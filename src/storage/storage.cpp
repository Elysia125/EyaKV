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
                 const std::optional<unsigned int> &data_flush_interval,
                 const DataFlushStrategy &data_flush_strategy) : data_dir_(data_dir),
                                                                 enable_wal_(enable_wal),
                                                                 read_only_(read_only),
                                                                 sstable_merge_threshold_(sstable_merge_threshold),
                                                                 data_flush_interval_(data_flush_interval),
                                                                 data_flush_strategy_(data_flush_strategy)
{
    // 确保数据目录存在
    if (!std::filesystem::exists(data_dir_))
    {
        std::filesystem::create_directories(data_dir_);
    }
    if (data_flush_interval.has_value())
    {
        if (data_flush_interval.value() <= 0)
        {
            data_flush_interval_ = std::nullopt;
        }
        else if ((data_flush_interval.value() <= 1000 && data_flush_strategy == DataFlushStrategy::BACKGROUND_THREAD) || data_flush_strategy == DataFlushStrategy::IMMEDIATE_ON_WRITE)
        {
            LOG_WARN("Data flush interval too low for BACKGROUND_THREAD strategy or IMMEDIATE_ON_WRITE strategy. Wal will be disabled.");
            enable_wal_ = false;
        }
    }
    // 初始化 MemTable
    memtable_ = std::make_unique<MemTable>(memtable_size,
                                           skiplist_max_level,
                                           skiplist_probability,
                                           skiplist_max_node_count);

    // 初始化 WAL
    wal_ = std::make_unique<Wal>(wal_dir,
                                 wal_file_size,
                                 max_wal_file_count);

    // 恢复数据
    Recover();

    // 启动后台数据刷新线程（如果配置了）
    if (((data_flush_strategy_ == DataFlushStrategy::BACKGROUND_THREAD && data_flush_interval_.has_value()) || enable_wal_) && !read_only_)
    {
        background_flush_thread_running_ = true;
        StartBackgroundFlushThread();
    }
}

Storage::~Storage() = default;

void Storage::Recover()
{
    if (wal_ && memtable_)
    {
        bool success = wal_->Recover(memtable_.get());
        if (!success)
        {
            LOG_ERROR("Storage: WAL recovery failed.");
        }
    }
}

bool Storage::Put(const std::string &key, const std::string &value)
{
    if (read_only_)
    {
        LOG_ERROR("Storage is in read-only mode. Put operation is not allowed.");
        return false;
    }
    // 1. 写 WAL
    if (enable_wal_ && !wal_->AppendPut(key, value))
    {
        return false;
    }
    // 2. 写 MemTable
    memtable_->put(key, value);
    return true;
}

std::optional<std::string> Storage::Get(const std::string &key) const
{
    // 1. 查 MemTable
    auto result = memtable_->get(key);
    if (result.has_value())
    {
        return result;
    }

    // TODO: 查 SSTable
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
    if (enable_wal_ && !wal_->AppendDelete(key))
    {
        return false;
    }
    // 2. 更新 MemTable
    memtable_->remove(key);
    return true;
}

void Storage::StartBackgroundFlushThread()
{
    LOG_INFO("Background flush thread starting....");

    std::thread bg_flush_thread(BackgroundFlushTask, this);
    bg_flush_thread.detach();
    LOG_INFO("Background flush thread started completely.");
}

void Storage::BackgroundFlushTask()
{
    if (!enable_wal_)
    {
        int sleep_interval = data_flush_interval_.value_or(1000);
        while (background_flush_thread_running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_interval));
            // 刷新 MemTable 到 SSTable (TODO)
        }
    }
    else if (data_flush_strategy_ != DataFlushStrategy::BACKGROUND_THREAD)
    {
        int sleep_interval = 1000; // 默认1秒
        while (background_flush_thread_running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_interval));
            if (wal_)
            {
                wal_->Sync();
            }
        }
    }
    else
    {
        std::string interval_str = std::to_string(data_flush_interval_.value());
        if (interval_str.substr(interval_str.length() - 3) == "000")
        {
            int end = data_flush_interval_.value() / 1000;
            int count = 0;
            while (background_flush_thread_running_)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (wal_)
                {
                    wal_->Sync();
                }
                count++;
                if (count >= end)
                {
                    // 执行数据刷盘操作(TODO)
                    count = 0;
                }
            }
        }
        else
        {
            double rate = data_flush_interval_.value() / 1000.0;
            int current = 0;
            while (background_flush_thread_running_)
            {
                if (rate - current < 1.0)
                {
                    int ms = static_cast<int>((rate - current) * 1000);
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    // 执行数据刷盘操作(TODO)

                    // 补足间隔
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 - ms));
                    current = 0;
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (wal_)
                {
                    wal_->Sync();
                }
                current += 1;
            }
        }
    }
}