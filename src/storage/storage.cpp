#include <filesystem>
#include <iostream>
#include <thread>
#include "logger/logger.h"
#include "storage/processors/structure_processors.h"

const auto remove_evalue = [](EValue &value) -> EValue &
{
        value.deleted = true;
        return value; };

Storage::Storage(const std::string &data_dir,
                 const std::string &wal_dir,
                 const bool &read_only,
                 const bool &enable_wal,
                 const std::optional<unsigned int> &wal_flush_interval,
                 const WALFlushStrategy &wal_flush_strategy,
                 const size_t &memtable_size,
                 const size_t &skiplist_max_level,
                 const double &skiplist_probability,
                 const size_t &skiplist_max_node_count,
                 const SSTableMergeStrategy &sstable_merge_strategy,
                 const unsigned int &sstable_merge_threshold,
                 const unsigned int &sstable_zero_level_size,
                 const double &sstable_level_size_ratio) : data_dir_(data_dir),
                                                           enable_wal_(enable_wal),
                                                           read_only_(read_only),
                                                           memtable_size_(memtable_size),
                                                           skiplist_max_level_(skiplist_max_level),
                                                           skiplist_probability_(skiplist_probability),
                                                           skiplist_max_node_count_(skiplist_max_node_count),
                                                           wal_flush_interval_(wal_flush_interval),
                                                           wal_flush_strategy_(wal_flush_strategy)
{
    // 确保数据目录存在
    if (!std::filesystem::exists(data_dir_))
    {
        std::filesystem::create_directories(data_dir_);
    }

    // 设置 SSTable 目录
    sstable_dir_ = PathUtils::CombinePath(data_dir_, "sstable");
    if (!std::filesystem::exists(sstable_dir_))
    {
        std::filesystem::create_directories(sstable_dir_);
    }

    if (wal_flush_strategy_ == WALFlushStrategy::BACKGROUND_THREAD && !wal_flush_interval_.has_value())
    {
        LOG_WARN("WAL flush interval not set for BACKGROUND_THREAD strategy. Using default 1000 ms.");
        wal_flush_interval_ = 1000; // 默认1秒
    }

    // 初始化 MemTable
    memtable_ = create_new_memtable();

    // 初始化 SSTable 管理器
    sstable_manager_ = std::make_unique<SSTableManager>(sstable_dir_,
                                                        sstable_merge_strategy,
                                                        sstable_merge_threshold,
                                                        sstable_zero_level_size,
                                                        sstable_level_size_ratio);

    // 初始化 WAL
    if (enable_wal_)
    {
        wal_ = std::make_unique<Wal>(wal_dir, wal_flush_strategy_ == WALFlushStrategy::IMMEDIATE_ON_WRITE);
    }
    else
    {
        current_wal_filename_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }
    // 恢复数据
    init_command_handlers();
    recover();

    start_background_flush_thread();
    LOG_INFO("Storage engine initialized. Data dir:%s ", data_dir_.c_str());
}

Storage::~Storage()
{
    close();
}

void Storage::close()
{
    if (closed_.exchange(true))
    {
        return; // 已经关闭过了
    }

    LOG_INFO("Closing storage engine...");

    // 停止后台线程
    stop_background_flush_thread();

    // 强制刷新所有数据到磁盘
    if (!read_only_)
    {
        force_flush();
    }

    // 关闭 WAL
    if (wal_)
    {
        wal_->sync();
    }

    LOG_INFO("Storage engine closed.");
}

std::unique_ptr<MemTable<std::string, EValue>> Storage::create_new_memtable()
{
    return std::make_unique<MemTable<std::string, EValue>>(
        memtable_size_,
        skiplist_max_level_,
        skiplist_probability_,
        skiplist_max_node_count_,
        std::nullopt,
        calculateStringSize,
        estimateEValueSize);
}

void Storage::recover()
{
    if (wal_)
    {
        std::unique_lock<std::shared_mutex> lock(immutable_mutex_);
        bool success = wal_->recover([this](std::string filename, uint8_t type, std::string key, std::string payload)
                                     {
            if (immutable_memtables_.find(filename) == immutable_memtables_.end())
            {
                immutable_memtables_[filename] = create_new_memtable();
            }
            auto &memtable = immutable_memtables_[filename];
            if(type == LogType::kRemove){
                memtable->handle_value(key, remove_evalue);
            }else if(type == LogType::kExpire){
                uint64_t expire_time = std::stoull(payload);
                set_key_expire(key, expire_time);
            }
            else if (processors_.find(type) != processors_.end())
            {
                processors_[type]->recover(memtable.get(), type, key, payload);
            }
            else
            {
                LOG_ERROR("Unknown log type: %d", type);
            } });
        if (!success)
        {
            LOG_ERROR("Storage: WAL recovery failed.");
        }
        else
        {
            LOG_INFO("Storage: WAL recovery completed.");
            if (immutable_memtables_.empty())
            {
                return;
            }
            // 判断最后一个（最新的）memtable是否需要flush
            auto last_memtable = immutable_memtables_.rbegin();
            if (!last_memtable->second->should_flush() && !read_only_)
            {
                memtable_ = std::move(last_memtable->second);
                if (wal_ && enable_wal_)
                {
                    wal_->open_wal_file(last_memtable->first);
                    current_wal_filename_ = last_memtable->first;
                }
                immutable_memtables_.erase(last_memtable->first);
            }
            else if (!read_only_ && enable_wal_ && wal_)
            {
                current_wal_filename_ = wal_->open_wal_file();
            }
            if (immutable_memtables_.empty())
            {
                return;
            }
            // 通知后台线程进行 Flush
            {
                std::lock_guard<std::mutex> lock(flush_mutex_);
                flush_cv_.notify_one();
            }
        }
    }
}

bool Storage::write_memtable(const std::string &key, EValue &value)
{
    try
    {
        memtable_->put(key, value);

        // 3. 检查是否需要 Flush
        if (memtable_->should_flush())
        {
            LOG_INFO("MemTable reached size limit, rotating...");
            rotate_memtable();
        }
    }
    catch (const std::overflow_error &e)
    {
        LOG_INFO("Start memtable rotating,because of:%s", e.what());
        rotate_memtable();
        memtable_->put(key, value);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Exception caught while putting key: %s, error: %s", key, e.what());
        return false;
    }

    return true;
}

std::optional<EyaValue> Storage::get(const std::string &key) const
{
    // 1. 查 MemTable（最新数据）
    std::optional<EValue> result;
    try
    {
        result = memtable_->handle_value(key, [](EValue &value) -> EValue &
                                         { 
                                            if(value.is_deleted()||value.is_expired()){
                                                throw std::out_of_range("key not found");
                                            }
                                            return value; });
        if (result.has_value())
        {
            return result->value;
        }
    }
    catch (const std::out_of_range &e)
    {
        LOG_INFO("key not found in memtable:%s", key);
    }

    // 2. 查 Immutable MemTables
    result = get_from_immutable_memtables(key);
    if (result.has_value())
    {
        if (result->is_expired() || result->is_deleted())
        {
            return std::nullopt;
        }
        return result->value;
    }

    // 3. 查 SSTable
    if (sstable_manager_)
    {
        EValue value;
        if (sstable_manager_->get(key, &value))
        {
            if (value.is_expired() || value.is_deleted())
            {
                return std::nullopt;
            }
            return value.value;
        }
    }

    return std::nullopt;
}

std::optional<EValue> Storage::get_from_immutable_memtables(const std::string &key) const
{
    std::shared_lock<std::shared_mutex> lock(immutable_mutex_);

    // 从最新到最旧查询 Immutable MemTables
    for (auto it = immutable_memtables_.begin(); it != immutable_memtables_.end(); ++it)
    {
        auto result = it->second->get(key);
        if (result.has_value())
        {
            return result;
        }
    }

    return std::nullopt;
}

bool Storage::contains(const std::string &key) const
{
    return get(key).has_value();
}

std::vector<std::pair<std::string, EyaValue>> Storage::range(
    const std::string &start_key,
    const std::string &end_key) const
{
    std::map<std::string, EValue> merged_results;

    // 1. 从 SSTable 获取范围数据（最旧的数据）
    if (sstable_manager_)
    {
        merged_results = sstable_manager_->range_query(start_key, end_key);
    }
    // 2. 从 Immutable MemTables 获取并覆盖
    {
        std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
        for (auto it = immutable_memtables_.rbegin(); it != immutable_memtables_.rend(); ++it)
        {
            auto entries = it->second->get_all_entries();
            for (const auto &[k, v] : entries)
            {
                if (k >= start_key && k <= end_key)
                {
                    merged_results[k] = v;
                }
            }
        }
    }

    // 3. 从 MemTable 获取并覆盖（最新的数据）
    auto entries = memtable_->get_all_entries();
    for (const auto &[k, v] : entries)
    {
        if (k >= start_key && k <= end_key)
        {
            merged_results[k] = v;
        }
    }

    // 转换为 vector
    std::vector<std::pair<std::string, EyaValue>> result;
    result.reserve(merged_results.size());
    for (auto &[k, v] : merged_results)
    {
        if (v.is_deleted() || v.is_expired())
        {
            continue;
        }
        result.emplace_back(k, std::move(v.value));
    }

    return result;
}

void Storage::rotate_memtable()
{
    // 将当前 MemTable 转换为 Immutable
    {
        std::unique_lock<std::shared_mutex> lock(immutable_mutex_);
        immutable_memtables_[current_wal_filename_] = std::move(memtable_);
    }
    if (enable_wal_ && wal_)
    {
        current_wal_filename_ = wal_->open_wal_file();
    }
    else
    {
        current_wal_filename_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }
    // 创建新的 MemTable
    memtable_ = create_new_memtable();

    // 通知后台线程进行 Flush
    {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        flush_cv_.notify_one();
    }
}

void Storage::force_flush()
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Flush operation is not allowed.");
        return;
    }

    // 将当前 MemTable 转换为 Immutable（如果有数据）
    if (memtable_->size() > 0)
    {
        rotate_memtable();
    }

    // Flush 所有 Immutable MemTables
    flush_memtable_to_sstable();
}

void Storage::flush_memtable_to_sstable()
{
    // Flush 每个 MemTable 到 SSTable
    for (auto it = immutable_memtables_.begin(); it != immutable_memtables_.end();)
    {
        std::string filename = it->first;
        auto &imm = it->second;
        if (imm->size() == 0)
        {
            ++it; // 空 MemTable，跳过并继续下一个
            continue;
        }

        auto entries = imm->get_all_entries();

        if (sstable_manager_)
        {
            auto meta = sstable_manager_->create_new_sstable(entries);
            if (meta.has_value())
            {
                LOG_INFO("Flushed MemTable to SSTable: %s with %s entries", meta->filepath, std::to_string(meta->entry_count));
                if (enable_wal_ && wal_)
                {
                    wal_->clear(filename);
                }
                it = immutable_memtables_.erase(it); // erase 返回下一个有效迭代器
            }
            else
            {
                LOG_ERROR("Failed to flush MemTable to SSTable");
                ++it; // flush 失败，跳过继续下一个
            }
        }
        else
        {
            ++it; // 没有 sstable_manager_，跳过继续下一个
        }
    }
}

void Storage::start_background_flush_thread()
{
    LOG_INFO("Background flush thread starting....");
    background_flush_thread_running_ = true;
    flush_thread_ = std::thread(&Storage::background_flush_task, this);
    flush_thread_.detach();

    LOG_INFO("Background flush thread started completely.");
}

void Storage::stop_background_flush_thread()
{
    if (!background_flush_thread_running_.exchange(false))
    {
        return; // 已经停止了
    }
    background_flush_thread_running_ = false;
    // 唤醒后台线程
    {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        flush_cv_.notify_all();
    }

    // 等待线程结束
    if (flush_thread_.joinable())
    {
        flush_thread_.join();
    }

    LOG_INFO("Background flush thread stopped.");
}

void Storage::background_flush_task()
{
    bool need_time_out = wal_flush_strategy_ == WALFlushStrategy::BACKGROUND_THREAD && enable_wal_ && !read_only_;
    while (background_flush_thread_running_)
    {
        // 等待通知或超时
        {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            if (need_time_out)
            {
                flush_cv_.wait_for(lock,
                                   std::chrono::milliseconds(wal_flush_interval_.value_or(1000)),
                                   [this]
                                   {
                                       return !background_flush_thread_running_ ||
                                              !immutable_memtables_.empty();
                                   });
            }
            else
            {
                flush_cv_.wait(lock, [this]
                               { return !background_flush_thread_running_ || !immutable_memtables_.empty(); });
            }
        }

        if (!background_flush_thread_running_)
        {
            break;
        }

        // 执行 Flush
        bool has_immutable = false;
        {
            std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
            has_immutable = !immutable_memtables_.empty();
        }

        if (has_immutable)
        {
            flush_memtable_to_sstable();
        }

        // 同步 WAL
        if (wal_)
        {
            wal_->sync();
        }
    }
}

Storage::Stats Storage::get_stats() const
{
    Stats stats;

    // MemTable 统计
    stats.memtable_size = memtable_->memory_usage();
    stats.memtable_count = memtable_->size();

    // Immutable MemTable 统计
    {
        std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
        stats.immutable_memtable_count = immutable_memtables_.size();
    }

    // SSTable 统计
    if (sstable_manager_)
    {
        stats.sstable_count = sstable_manager_->get_sstable_count();
        stats.total_sstable_size = sstable_manager_->get_total_size();
    }
    else
    {
        stats.sstable_count = 0;
        stats.total_sstable_size = 0;
    }
}

void Storage::register_processor(std::shared_ptr<ValueProcessor> processor)
{
    for (auto type : processor->get_supported_types())
    {
        processors_[type] = processor;
    }
}

void Storage::init_command_handlers()
{
    register_processor(std::make_shared<StringProcessor>());
    register_processor(std::make_shared<SetProcessor>());
    register_processor(std::make_shared<ZSetProcessor>());
    register_processor(std::make_shared<DequeProcessor>());
    register_processor(std::make_shared<MapProcessor>());
}

void Storage::set_expire(const std::string &key, uint64_t alive_time)
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Set expire operation is not allowed.");
        throw std::runtime_error("Storage is in read-only mode. Set expire operation is not allowed.");
    }
    uint64_t expire_time = alive_time == 0 ? 0 : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + alive_time;
    if (enable_wal_ && wal_)
    {
        std::string payload = std::to_string(expire_time);
        wal_->append_log(LogType::kExpire, key, payload);
    }
    set_key_expire(key, expire_time);
}

void Storage::set_key_expire(const std::string &key, uint64_t expire_time)
{
    try
    {
        memtable_->handle_value(key, [&](EValue &v) -> EValue &
                                {
        v.expire_time = expire_time;
        return v; });
    }
    catch (const std::out_of_range &)
    {
        auto v = get(key);
        if (!v.has_value())
        {
            LOG_ERROR("key not found when set expire.");
            throw std::runtime_error("key not found");
        }
        EValue value(v.value());
        value.expire_time = expire_time;
        try
        {
            memtable_->put(key, value);
        }
        catch (const std::overflow_error &)
        {
            rotate_memtable();
            memtable_->put(key, value);
        }
    }
}