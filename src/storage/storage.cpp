#include <filesystem>
#include <iostream>
#include <thread>
#include "logger/logger.h"
#include "storage/processors/structure_processors.h"
#include "storage/storage.h"
#include "common/types/operation_type.h"

const auto remove_evalue = [](EValue &value) -> EValue &
{
        value.deleted = true;
        return value; };

std::unique_ptr<Storage> Storage::instance_ = nullptr;
bool Storage::is_init_ = false;
Storage::Storage(const std::string &data_dir,
                 const std::string &wal_dir,
                 const bool &read_only,
                 const bool &enable_wal,
                 const std::optional<unsigned int> &wal_flush_interval,
                 const WALFlushStrategy &wal_flush_strategy,
                 const size_t &memtable_size,
                 const size_t &skiplist_max_level,
                 const double &skiplist_probability,
                 const SSTableMergeStrategy &sstable_merge_strategy,
                 const unsigned int &sstable_merge_threshold,
                 const unsigned int &sstable_zero_level_size,
                 const double &sstable_level_size_ratio) : data_dir_(data_dir),
                                                           enable_wal_(enable_wal),
                                                           read_only_(read_only),
                                                           memtable_size_(memtable_size),
                                                           skiplist_max_level_(skiplist_max_level),
                                                           skiplist_probability_(skiplist_probability),
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

    // 初始化 SSTable 管理器
    sstable_manager_ = std::make_unique<SSTableManager>(sstable_dir_,
                                                        sstable_merge_strategy,
                                                        sstable_merge_threshold,
                                                        sstable_zero_level_size,
                                                        sstable_level_size_ratio);

    // 初始化 WAL
    wal_ = std::make_unique<Wal>(wal_dir, wal_flush_strategy_ == WALFlushStrategy::IMMEDIATE_ON_WRITE);

    init_command_handlers();
    // 恢复数据
    recover();

    // 初始化 MemTable
    if (!memtable_)
    {
        memtable_ = create_new_memtable();
    }
    if (current_wal_filename_ == "")
    {
        if (enable_wal_)
        {
            current_wal_filename_ = wal_->open_wal_file();
        }
        else
        {
            current_wal_filename_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        }
    }
    start_background_flush_thread();
    is_init_ = true;
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

std::unique_ptr<MemTable> Storage::create_new_memtable()
{
    return std::make_unique<MemTable>(
        memtable_size_,
        skiplist_max_level_,
        skiplist_probability_);
}

void Storage::recover()
{
    if (wal_)
    {
        LOG_INFO("Storage::Recover: Starting WAL recovery...");
        // 初始化memtable_
        memtable_ = create_new_memtable();
        memtable_->cancel_size_limit();
        current_wal_filename_ = "";
        LOG_INFO("Storage::Recover: Locking immutable_mutex_...");
        // std::unique_lock<std::shared_mutex> lock(immutable_mutex_);
        LOG_INFO("Storage::Recover: Calling wal_->recover()...");
        bool success = wal_->recover([this](std::string filename, uint8_t type, std::string key, std::string payload)
                                     {
            LOG_DEBUG("Storage::Recover callback: Processing record - filename: %s, type: %d, key: %s",
                      filename.c_str(), type, key.c_str());
            if(filename != current_wal_filename_){
                if(current_wal_filename_!=""){
                    LOG_DEBUG("Storage::Recover callback: Moving memtable to immutable, current: %s, new: %s",
                              current_wal_filename_.c_str(), filename.c_str());
                    immutable_memtables_[current_wal_filename_] = std::move(memtable_);
                    memtable_ = create_new_memtable();
                    memtable_->cancel_size_limit();
                }
                current_wal_filename_ = filename;
            }
            try{
                if(type == OperationType::kRemove){
                    std::vector<std::string> keys{key};
                    LOG_DEBUG("Storage::Recover callback: Processing REMOVE for key: %s", key.c_str());
                    remove(keys);
                }else if(type == OperationType::kExpire){
                    uint64_t expire_time = std::stoull(payload);
                    LOG_DEBUG("Storage::Recover callback: Processing EXPIRE for key: %s, expire_time: %llu",
                              key.c_str(), expire_time);
                    set_key_expire(key, expire_time);
                }
                else if (processors_.find(type) != processors_.end())
                {
                    LOG_DEBUG("Storage::Recover callback: Processing custom type: %d for key: %s", type, key.c_str());
                    if(!processors_[type]->recover(this, type, key, payload)){
                        LOG_ERROR("Storage: WAL recovery failed for type: %d, key: %s, payload: %s", type, key.c_str(), payload.c_str());
                    }
                }
                else
                {
                    LOG_ERROR("Storage::Recover callback: Unknown log type: %d", type);
                }
            }catch(std::exception& e){
                LOG_WARN("Storage::Recover callback: Exception occurred for type %d key %s payload %s, exception: %s",
                         type, key.c_str(), payload.c_str(), e.what());
            } });
        LOG_INFO("Storage::Recover: WAL recover() returned, success: %d", success);
        if (!success)
        {
            LOG_ERROR("Storage: WAL recovery failed.");
        }
        else
        {
            LOG_INFO("Storage: WAL recovery completed successfully.");
            memtable_->set_size_limit(memtable_size_);
            if (memtable_->should_flush() || !enable_wal_)
            {
                LOG_INFO("Storage::Recover: Flushing memtable to immutable...");
                immutable_memtables_[current_wal_filename_] = std::move(memtable_);
                memtable_ = create_new_memtable();
                if (enable_wal_)
                {
                    current_wal_filename_ = wal_->open_wal_file();
                }
                else
                {
                    current_wal_filename_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                }
            }
            else
            {
                LOG_INFO("Storage::Recover: Opening WAL file: %s", current_wal_filename_.c_str());
                wal_->open_wal_file(current_wal_filename_);
            }
            if (immutable_memtables_.empty())
            {
                LOG_INFO("Storage::Recover: No immutable memtables to flush, returning.");
                return;
            }
            // 通知后台线程进行 Flush
            LOG_INFO("Storage::Recover: Notifying background flush thread...");
            {
                std::lock_guard<std::mutex> lock(flush_mutex_);
                flush_cv_.notify_one();
            }
            LOG_INFO("Storage::Recover: Recovery process completed.");
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
    std::optional<EValue> result;
    LOG_INFO("Get from latest memtable: %s", key.c_str());
    if (get_from_latest(key, result))
    {
        return result->value;
    }
    LOG_WARN("Get from latest memtable failed, try to get from old memtable: %s", key.c_str());
    LOG_INFO("Get from old memtable: %s", key.c_str());
    if (get_from_old(key, result))
    {
        return result->value;
    }
    LOG_WARN("Get from old memtable failed, key: %s not found", key.c_str());
    return std::nullopt;
}

bool Storage::get_from_latest(const std::string &key, std::optional<EValue> &value) const
{
    std::optional<EValue> result;
    try
    {
        result = memtable_->get(key);
        if (result.has_value())
        {
            if (result->is_expired() || result->is_deleted())
            {
                return false;
            }
            value = result.value();
            return true;
        }
    }
    catch (const std::out_of_range &e)
    {
        return false;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Exception caught while getting key: %s, error: %s", key, e.what());
        return false;
    }
    return false;
}

bool Storage::get_from_old(const std::string &key, std::optional<EValue> &value) const
{
    std::optional<EValue> result;
    // 查 Immutable MemTables
    result = get_from_immutable_memtables(key);
    if (result.has_value())
    {
        if (result->is_expired() || result->is_deleted())
        {
            return false;
        }
        value = result.value();
        return true;
    }

    // 查 SSTable
    if (sstable_manager_)
    {
        EValue value;
        if (sstable_manager_->get(key, &value))
        {
            if (value.is_expired() || value.is_deleted())
            {
                return false;
            }
            value = value.value;
            return true;
        }
    }
    return false;
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

    return stats;
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
    register_processor(std::make_shared<HashProcessor>());
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
        wal_->append_log(OperationType::kExpire, key, payload);
    }
    set_key_expire(key, expire_time);
}

void Storage::set_key_expire(const std::string &key, uint64_t expire_time)
{
    try
    {
        memtable_->handle_value(key, [&](EValue &v) -> EValue &
                                {
                                    if(v.is_deleted()||v.is_expired()){
                                        return v;
                                    }
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

uint32_t Storage::remove(std::vector<std::string> &keys)
{
    if (read_only_)
    {
        LOG_WARN("Storage: remove failed, read only mode");
        throw std::runtime_error("Remove key failed, read only mode");
    }
    if (keys.empty())
    {
        throw std::runtime_error("Remove key failed, missing key");
    }
    uint32_t count = 0;
    for (auto &key : keys)
    {
        if (enable_wal_ && wal_ && !wal_->append_log(OperationType::kRemove, std::forward<decltype(key)>(key), ""))
        {
            LOG_ERROR("Storage: remove key %s failed, append log failed",
                      key.c_str());
            continue;
        }
        try
        {
            memtable_->handle_value(key, remove_evalue);
            ++count;
        }
        catch (const std::out_of_range &)
        {
            // 插入一个delete值
            EValue delete_value;
            delete_value.deleted = true;
            memtable_->put(key, delete_value);
            ++count;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Storage: remove key %s failed, %s", key.c_str(), e.what());
        }
    }

    return count;
}
Response Storage::execute(uint8_t type, std::vector<std::string> &args)
{
    std::string args_str;
    for (const auto &arg : args)
        args_str += arg + " ";
    LOG_DEBUG("Storage::execute: type=%d, args=[%s]", type, args_str.c_str());
    try
    {
        if (type == OperationType::kRemove)
        {
            if (args.empty())
            {
                return Response::error("missing key");
            }
            return Response::success(std::to_string(remove(args)));
        }
        else if (type == OperationType::kExists)
        {
            if (args.empty())
            {
                return Response::error("missing key");
            }
            else if (args.size() > 1)
            {
                return Response::error("too many arguments");
            }
            return Response::success(std::string(contains(args[0]) ? "1" : "0"));
        }
        else if (type == OperationType::kRange)
        {
            if (args.size() < 2)
            {
                return Response::error("missing key");
            }
            if (args.size() > 2)
            {
                return Response::error("too many arguments");
            }
            return Response::success(range(args[0], args[1]));
        }
        else if (type == OperationType::kExpire)
        {
            if (args.size() <= 1)
            {
                return Response::error("missing key");
            }
            if (args.size() > 2)
            {
                return Response::error("too many arguments");
            }
            uint64_t expire_time = std::stoull(args[1]);
            set_expire(args[0], expire_time);
            return Response::success(std::string("1"));
        }
        else if (type == OperationType::kGet)
        {
            if (args.size() == 0)
            {
                return Response::error("missing key");
            }
            if (args.size() > 1)
            {
                return Response::error("too many arguments");
            }
            auto value = get(args[0]);
            ResponseData data;
            if (value.has_value())
            {
                data = value.value();
            }
            return Response::success(data);
        }
        else
        {
            auto processor = get_processor(type);
            if (processor)
            {
                try
                {
                    return processor->execute(this, type, args);
                }
                catch (const std::exception &e)
                {
                    return Response::error(e.what());
                }
            }
            return Response::error("unknown command");
        }
    }
    catch (const std::runtime_error &e)
    {
        return Response::error(e.what());
    }
    catch (const std::exception &e)
    {
        return Response::error("unknown error");
    }
}