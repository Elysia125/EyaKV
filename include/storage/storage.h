#ifndef STORAGE_H
#define STORAGE_H_

#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <shared_mutex>
#include "storage/memtable.h"
#include "storage/sstable.h"
#include "storage/wal.h"
#include "config/config.h"
#include <atomic>
#include <thread>
#include <condition_variable>
#include "common/common.h"


/**
 * @brief Storage 类是存储引擎的统一入口。
 *
 * 它负责管理内存中的 MemTable、磁盘上的 WAL 以及 SSTable。
 * 对外提供统一的 Put/Get/Delete 接口。
 * 
 * 数据写入流程：
 * 1. 写入 WAL (持久化保障)
 * 2. 写入 MemTable (内存索引)
 * 3. 当 MemTable 达到阈值时，转换为 Immutable MemTable
 * 4. 后台线程将 Immutable MemTable Flush 到 SSTable
 * 
 * 数据读取流程（LSM-tree）：
 * 1. 查询 MemTable
 * 2. 查询 Immutable MemTables（如果有）
 * 3. 查询 SSTable（按时间顺序，新的优先）
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
                     const std::optional<unsigned int> &wal_flush_interval = 1000,
                     const WALFlushStrategy &wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD);
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
     * 3. 如果 MemTable 满了，触发 Flush
     */
    bool Put(const std::string &key, const EyaValue &value);

    /**
     * @brief 读取数据。
     *
     * 流程（按优先级）：
     * 1. 查询 MemTable
     * 2. 查询 Immutable MemTables
     * 3. 查询 SSTable
     */
    std::optional<EyaValue> Get(const std::string &key) const;

    /**
     * @brief 删除数据。
     *
     * 在 LSM-tree 中，删除操作实际上是写入一个 Tombstone 标记。
     * 真正的删除发生在 Compaction 过程中。
     * 
     * 流程：
     * 1. 写入 WAL (Tombstone)
     * 2. 在 MemTable 中标记删除
     */
    bool Delete(const std::string &key);

    /**
     * @brief 检查 key 是否存在。
     * @param key 要检查的 key
     * @return 如果存在返回 true
     */
    bool Contains(const std::string &key) const;

    /**
     * @brief 范围查询 - 获取指定范围内的所有 KV 对。
     * @param start_key 起始 key（包含）
     * @param end_key 结束 key（包含）
     * @return 范围内的 KV 对列表（按 key 排序）
     */
    std::vector<std::pair<std::string, EyaValue>> Range(
        const std::string &start_key,
        const std::string &end_key) const;

    /**
     * @brief 强制将当前 MemTable Flush 到 SSTable。
     * 通常由后台线程自动触发，但也可以手动调用。
     */
    void ForceFlush();

    /**
     * @brief 获取存储引擎的统计信息。
     */
    struct Stats {
        size_t memtable_size;              // 当前 MemTable 大小
        size_t memtable_count;             // MemTable 中的条目数
        size_t immutable_memtable_count;   // Immutable MemTable 数量
        size_t sstable_count;              // SSTable 文件数量
        size_t total_sstable_size;         // SSTable 总大小
    };
    Stats GetStats() const;

    /**
     * @brief 关闭存储引擎，确保所有数据都持久化。
     */
    void Close();

private:
    std::string data_dir_;
    std::string sstable_dir_;
    
    // 活跃的 MemTable（接受新写入）
    std::unique_ptr<MemTable<std::string, EyaValue>> memtable_;
    
    // Immutable MemTables（等待 Flush 到 SSTable）
    std::vector<std::unique_ptr<MemTable<std::string, EyaValue>>> immutable_memtables_;
    mutable std::shared_mutex immutable_mutex_;  // 保护 immutable_memtables_
    
    // SSTable 管理器
    std::unique_ptr<SSTableManager> sstable_manager_;
    
    // WAL
    std::unique_ptr<Wal> wal_;
    
    // 配置
    bool enable_wal_;
    bool read_only_;
    size_t memtable_size_;
    size_t skiplist_max_level_;
    double skiplist_probability_;
    size_t skiplist_max_node_count_;
    unsigned int sstable_merge_threshold_;
    WALFlushStrategy wal_flush_strategy_;
    std::optional<unsigned int> wal_flush_interval_;
    
    // 后台线程控制
    std::atomic<bool> background_flush_thread_running_{false};
    std::atomic<bool> closed_{false};
    std::thread flush_thread_;
    std::condition_variable flush_cv_;
    std::mutex flush_mutex_;

    // 初始化时恢复数据
    void Recover();

    // 后台 Flush 线程
    void StartBackgroundFlushThread();
    void StopBackgroundFlushThread();
    void BackgroundFlushTask();

    // 将 MemTable Flush 到 SSTable
    void FlushMemTableToSSTable();
    
    // 将当前 MemTable 转换为 Immutable
    void RotateMemTable();
    
    // 在 Immutable MemTables 中查找
    std::optional<EyaValue> GetFromImmutableMemTables(const std::string &key) const;
    
    // 创建新的 MemTable
    std::unique_ptr<MemTable<std::string, EyaValue>> CreateNewMemTable();
};
#endif // STORAGE_H

