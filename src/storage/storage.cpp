#include "storage/storage.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include "logger/logger.h"

Storage::Storage(const std::string &data_dir,
                 const std::string &wal_dir,
                 const bool &read_only,
                 const bool &enable_wal,
                 const unsigned long &wal_file_size,
                 const unsigned long &max_wal_file_count,
                 const unsigned int &wal_sync_interval,
                 const size_t &memtable_size,
                 const size_t &skiplist_max_level,
                 const double &skiplist_probability,
                 const size_t &skiplist_max_node_count,
                 const unsigned int &sstable_merge_threshold,
                 const std::optional<unsigned int> &wal_flush_interval,
                 const WALFlushStrategy &wal_flush_strategy) : data_dir_(data_dir),
                                                               enable_wal_(enable_wal),
                                                               read_only_(read_only),
                                                               memtable_size_(memtable_size),
                                                               skiplist_max_level_(skiplist_max_level),
                                                               skiplist_probability_(skiplist_probability),
                                                               skiplist_max_node_count_(skiplist_max_node_count),
                                                               sstable_merge_threshold_(sstable_merge_threshold),
                                                               wal_flush_interval_(wal_flush_interval),
                                                               wal_flush_strategy_(wal_flush_strategy)
{
    // 确保数据目录存在
    if (!std::filesystem::exists(data_dir_))
    {
        std::filesystem::create_directories(data_dir_);
    }
    
    // 设置 SSTable 目录
    sstable_dir_ = data_dir_ + "/sstables";
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
    memtable_ = CreateNewMemTable();

    // 初始化 SSTable 管理器
    sstable_manager_ = std::make_unique<SSTableManager>(sstable_dir_);

    // 初始化 WAL
    if (enable_wal_)
    {
        wal_ = std::make_unique<Wal>(wal_dir,
                                     wal_file_size,
                                     max_wal_file_count, 
                                     wal_flush_strategy_ == WALFlushStrategy::IMMEDIATE_ON_WRITE);
    }

    // 恢复数据
    Recover();

    // 启动后台数据刷新线程（如果配置了）
    if (wal_flush_strategy_ == WALFlushStrategy::BACKGROUND_THREAD && enable_wal_ && !read_only_)
    {
        background_flush_thread_running_ = true;
        StartBackgroundFlushThread();
    }
    
    LOG_INFO("Storage engine initialized. Data dir: " + data_dir_);
}

Storage::~Storage()
{
    Close();
}

void Storage::Close()
{
    if (closed_.exchange(true))
    {
        return; // 已经关闭过了
    }
    
    LOG_INFO("Closing storage engine...");
    
    // 停止后台线程
    StopBackgroundFlushThread();
    
    // 强制刷新所有数据到磁盘
    if (!read_only_)
    {
        ForceFlush();
    }
    
    // 关闭 WAL
    if (wal_)
    {
        wal_->Sync();
    }
    
    LOG_INFO("Storage engine closed.");
}

std::unique_ptr<MemTable<std::string, EyaValue>> Storage::CreateNewMemTable()
{
    return std::make_unique<MemTable<std::string, EyaValue>>(
        memtable_size_,
        skiplist_max_level_,
        skiplist_probability_,
        skiplist_max_node_count_
    );
}

void Storage::Recover()
{
    if (wal_ && memtable_)
    {
        bool success = wal_->Recover(memtable_.get());
        if (!success)
        {
            LOG_ERROR("Storage: WAL recovery failed.");
        }
        else
        {
            LOG_INFO("Storage: WAL recovery completed. MemTable size: " + 
                     std::to_string(memtable_->size()));
        }
    }
}

bool Storage::Put(const std::string &key, const EyaValue &value)
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Put operation is not allowed.");
        return false;
    }
    
    // 1. 写 WAL
    if (enable_wal_ && wal_ && !wal_->AppendPut(key, value))
    {
        LOG_ERROR("Failed to write to WAL for key: " + key);
        return false;
    }
    
    // 2. 写 MemTable
    memtable_->put(key, value);
    
    // 3. 检查是否需要 Flush
    if (memtable_->shouldFlush())
    {
        LOG_INFO("MemTable reached size limit, rotating...");
        RotateMemTable();
    }
    
    return true;
}

std::optional<EyaValue> Storage::Get(const std::string &key) const
{
    // 1. 查 MemTable（最新数据）
    auto result = memtable_->get(key);
    if (result.has_value())
    {
        // 检查是否是 tombstone（删除标记）
        // 在这个实现中，我们假设删除操作会直接从 MemTable 中移除
        return result;
    }

    // 2. 查 Immutable MemTables
    result = GetFromImmutableMemTables(key);
    if (result.has_value())
    {
        return result;
    }

    // 3. 查 SSTable
    if (sstable_manager_)
    {
        EyaValue value;
        if (sstable_manager_->Get(key, &value))
        {
            return value;
        }
    }
    
    return std::nullopt;
}

std::optional<EyaValue> Storage::GetFromImmutableMemTables(const std::string &key) const
{
    std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
    
    // 从最新到最旧查询 Immutable MemTables
    for (auto it = immutable_memtables_.rbegin(); it != immutable_memtables_.rend(); ++it)
    {
        auto result = (*it)->get(key);
        if (result.has_value())
        {
            return result;
        }
    }
    
    return std::nullopt;
}

bool Storage::Delete(const std::string &key)
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Delete operation is not allowed.");
        return false;
    }
    
    // 1. 写 WAL
    if (enable_wal_ && wal_ && !wal_->AppendDelete(key))
    {
        LOG_ERROR("Failed to write delete to WAL for key: " + key);
        return false;
    }
    
    // 2. 从 MemTable 中移除
    // 注意：在真正的 LSM-tree 实现中，删除应该写入一个 Tombstone 标记
    // 这里简化处理，直接移除
    memtable_->remove(key);
    
    return true;
}

bool Storage::Contains(const std::string &key) const
{
    return Get(key).has_value();
}

std::vector<std::pair<std::string, EyaValue>> Storage::Range(
    const std::string &start_key,
    const std::string &end_key) const
{
    std::map<std::string, EyaValue> merged_results;
    
    // 1. 从 SSTable 获取范围数据（最旧的数据）
    // TODO: 实现 SSTable 的范围查询并合并
    
    // 2. 从 Immutable MemTables 获取并覆盖
    {
        std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
        for (const auto& imm : immutable_memtables_)
        {
            auto entries = imm->getAllEntries();
            for (const auto& [k, v] : entries)
            {
                if (k >= start_key && k <= end_key)
                {
                    merged_results[k] = v;
                }
            }
        }
    }
    
    // 3. 从 MemTable 获取并覆盖（最新的数据）
    auto entries = memtable_->getAllEntries();
    for (const auto& [k, v] : entries)
    {
        if (k >= start_key && k <= end_key)
        {
            merged_results[k] = v;
        }
    }
    
    // 转换为 vector
    std::vector<std::pair<std::string, EyaValue>> result;
    result.reserve(merged_results.size());
    for (auto& [k, v] : merged_results)
    {
        result.emplace_back(k, std::move(v));
    }
    
    return result;
}

void Storage::RotateMemTable()
{
    // 将当前 MemTable 转换为 Immutable
    {
        std::unique_lock<std::shared_mutex> lock(immutable_mutex_);
        immutable_memtables_.push_back(std::move(memtable_));
    }
    
    // 创建新的 MemTable
    memtable_ = CreateNewMemTable();
    
    // 清空 WAL（新的 MemTable 从空开始）
    if (enable_wal_ && wal_)
    {
        wal_->Clear();
    }
    
    // 通知后台线程进行 Flush
    {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        flush_cv_.notify_one();
    }
}

void Storage::ForceFlush()
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Flush operation is not allowed.");
        return;
    }
    
    // 将当前 MemTable 转换为 Immutable（如果有数据）
    if (memtable_->size() > 0)
    {
        RotateMemTable();
    }
    
    // Flush 所有 Immutable MemTables
    FlushMemTableToSSTable();
}

void Storage::FlushMemTableToSSTable()
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Flush operation is not allowed.");
        return;
    }
    
    std::vector<std::unique_ptr<MemTable<std::string, EyaValue>>> to_flush;
    
    // 获取所有待 Flush 的 Immutable MemTables
    {
        std::unique_lock<std::shared_mutex> lock(immutable_mutex_);
        to_flush = std::move(immutable_memtables_);
        immutable_memtables_.clear();
    }
    
    // Flush 每个 MemTable 到 SSTable
    for (auto& imm : to_flush)
    {
        if (imm->size() == 0)
        {
            continue;
        }
        
        auto entries = imm->getAllEntries();
        
        if (sstable_manager_)
        {
            auto meta = sstable_manager_->CreateFromEntries(entries);
            if (meta.has_value())
            {
                LOG_INFO("Flushed MemTable to SSTable: " + meta->filepath + 
                         " with " + std::to_string(meta->entry_count) + " entries");
            }
            else
            {
                LOG_ERROR("Failed to flush MemTable to SSTable");
            }
        }
    }
}

void Storage::StartBackgroundFlushThread()
{
    LOG_INFO("Background flush thread starting....");

    flush_thread_ = std::thread(&Storage::BackgroundFlushTask, this);
    
    LOG_INFO("Background flush thread started completely.");
}

void Storage::StopBackgroundFlushThread()
{
    if (!background_flush_thread_running_.exchange(false))
    {
        return; // 已经停止了
    }
    
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

void Storage::BackgroundFlushTask()
{
    while (background_flush_thread_running_)
    {
        // 等待通知或超时
        {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            flush_cv_.wait_for(lock, 
                std::chrono::milliseconds(wal_flush_interval_.value_or(1000)),
                [this] { 
                    return !background_flush_thread_running_ || 
                           !immutable_memtables_.empty(); 
                });
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
            FlushMemTableToSSTable();
        }
        
        // 同步 WAL
        if (wal_)
        {
            wal_->Sync();
        }
    }
}

Storage::Stats Storage::GetStats() const
{
    Stats stats;
    
    // MemTable 统计
    stats.memtable_size = memtable_->memoryUsage();
    stats.memtable_count = memtable_->size();
    
    // Immutable MemTable 统计
    {
        std::shared_lock<std::shared_mutex> lock(immutable_mutex_);
        stats.immutable_memtable_count = immutable_memtables_.size();
    }
    
    // SSTable 统计
    if (sstable_manager_)
    {
        stats.sstable_count = sstable_manager_->GetSSTableCount();
        stats.total_sstable_size = sstable_manager_->GetTotalSize();
    }
    else
    {
        stats.sstable_count = 0;
        stats.total_sstable_size = 0;
    }
    
    return stats;
}