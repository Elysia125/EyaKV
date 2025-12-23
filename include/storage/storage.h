#pragma once

#include <string>
#include <memory>
#include <optional>
#include "storage/memtable.h"
#include "storage/wal.h"
#include "config/config.h"
#include <atomic>
/**
 * @brief Storage 类是存储引擎的统一入口。
 *
 * 它负责管理内存中的 MemTable、磁盘上的 WAL 以及未来的 SSTable。
 * 对外提供统一的 Put/Get/Delete 接口。
 */
class Storage
{
public:
    /**
     * @brief 构造函数。
     * @param data_dir 数据存储目录，用于存放 WAL 和 SSTable 文件。
     */
    explicit Storage(const std::string &data_dir,
                     const std::string &wal_dir,
                     const bool &read_only = false,
                     const bool &enable_wal = true,
                     const unsigned long &wal_file_size = 64 * 1024 * 1024,
                     const unsigned long &max_wal_file_count = 16,
                     const unsigned int &wal_sync_interval = 1000,
                     const size_t &memtable_size = 1024 * 1024 * 1024,
                     const size_t &skiplist_max_level = 16,
                     const double &skiplist_probability = 0.5,
                     const size_t &skiplist_max_node_count = 10000000,
                     const unsigned int &sstable_merge_threshold = 5,
                     const std::optional<unsigned int> &data_flush_interval = 1000,
                     const DataFlushStrategy &data_flush_strategy = DataFlushStrategy::BACKGROUND_THREAD);
    ~Storage();

    // 禁止拷贝
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    // 允许移动
    Storage(Storage &&) = default;
    Storage &operator=(Storage &&) = default;
    /**
     * @brief 写入数据。
     *
     * 流程：
     * 1. 写入 WAL (持久化)
     * 2. 写入 MemTable (内存索引)
     */
    bool Put(const std::string &key, const std::string &value);

    /**
     * @brief 读取数据。
     *
     * 流程：
     * 1. 查询 MemTable
     * 2. (TODO) 查询 Immutable MemTable
     * 3. (TODO) 查询 SSTable
     */
    std::optional<std::string> Get(const std::string &key) const;

    /**
     * @brief 删除数据。
     *
     * 流程：
     * 1. 写入 WAL (Tombstone)
     * 2. 在 MemTable 中标记删除或直接移除
     */
    bool Delete(const std::string &key);

private:
    std::string data_dir_;
    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<Wal> wal_;
    bool enable_wal_;
    bool read_only_;
    unsigned int sstable_merge_threshold_;
    std::optional<unsigned int> data_flush_interval_;
    DataFlushStrategy data_flush_strategy_;

    std::atomic<bool> background_flush_thread_running_{false};
    // 初始化时恢复数据
    void Recover();

    void StartBackgroundFlushThread();

    void StopBackgroundFlushThread()
    {
        background_flush_thread_running_ = false;
    }

    void BackgroundFlushTask();
};
