#ifndef STORAGE_H_
#define STORAGE_H_

#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <set>
#include <shared_mutex>
#include <filesystem>
#include "storage/memtable.h"
#include "storage/sstable.h"
#include "storage/wal.h"
#include "config/config.h"
#include <atomic>
#include <thread>
#include <condition_variable>
#include <condition_variable>
#include "common/types/value.h"
#include "storage/processors/processor.h"
#include "network/protocol/protocol.h"
#include "storage/node.h"
#include "common/base/export.h"

// Forward declaration
class ValueProcessor;
class StringProcessor;
class VectorProcessor;
class SetProcessor;
class ZSetProcessor;

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
class EYAKV_STORAGE_API Storage
{
private:
    /**
     * @brief 构造函数。
     * @param data_dir 数据存储目录，用于存放 WAL 和 SSTable 文件。
     */
    explicit Storage(const std::string &data_dir,
                     const std::string &wal_dir,
                     const bool &read_only = false,
                     const bool &enable_wal = true,
                     const std::optional<uint32_t> &wal_flush_interval = 1000,
                     const WALFlushStrategy &wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD,
                     const size_t &memtable_size = 1024 * 1024 * 1024,
                     const size_t &skiplist_max_level = 16,
                     const double &skiplist_probability = 0.5,
                     const SSTableMergeStrategy &sstable_merge_strategy = SSTableMergeStrategy::SIZE_TIERED_COMPACTION,
                     const uint32_t &sstable_merge_threshold = 5,
                     const uint64_t &sstable_zero_level_size = 10,
                     const double &sstable_level_size_ratio = 10.0);

public:
    ~Storage();

    // 禁止拷贝
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    // 允许移动
    Storage(Storage &&) = default;
    Storage &operator=(Storage &&) = default;

    // 获取静态单例(双重检查锁)
    static Storage *get_instance()
    {
        return instance_.get();
    }
    // 初始化单例
    static void init(const std::string &data_dir,
                     const std::string &wal_dir,
                     const bool &read_only = false,
                     const bool &enable_wal = true,
                     const std::optional<uint32_t> &wal_flush_interval = 1000,
                     const WALFlushStrategy &wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD,
                     const size_t &memtable_size = 1024 * 1024,
                     const size_t &skiplist_max_level = 16,
                     const double &skiplist_probability = 0.5,
                     const SSTableMergeStrategy &sstable_merge_strategy = SSTableMergeStrategy::SIZE_TIERED_COMPACTION,
                     const uint32_t &sstable_merge_threshold = 5,
                     const uint32_t &sstable_zero_level_size = 10,
                     const double &sstable_level_size_ratio = 10.0)
    {
        if (instance_ == nullptr)
        {
            instance_ = std::unique_ptr<Storage>(new Storage(data_dir, wal_dir,
                                                             read_only, enable_wal,
                                                             wal_flush_interval,
                                                             wal_flush_strategy,
                                                             memtable_size, skiplist_max_level,
                                                             skiplist_probability,
                                                             sstable_merge_strategy, sstable_merge_threshold,
                                                             sstable_zero_level_size, sstable_level_size_ratio));
        }
    }

    static bool is_init()
    {
        return is_init_;
    }
    /**
     * @brief 读取数据。
     *
     * 流程（按优先级）：
     * 1. 查询 MemTable
     * 2. 查询 Immutable MemTables
     * 3. 查询 SSTable
     */
    std::optional<EyaValue> get(const std::string &key) const;

    Response execute(uint8_t type, std::vector<std::string> &args);
    /**
     * @brief 注册自定义命令处理器
     */
    void register_processor(std::shared_ptr<ValueProcessor> processor);

    /**
     * @brief 强制将当前 MemTable Flush 到 SSTable。
     * 通常由后台线程自动触发，但也可以手动调用。
     */
    void force_flush();

    /**
     * @brief 获取存储引擎的统计信息。
     */
    struct Stats
    {
        size_t memtable_size;            // 当前 MemTable 大小
        size_t memtable_count;           // MemTable 中的条目数
        size_t immutable_memtable_count; // Immutable MemTable 数量
        size_t sstable_count;            // SSTable 文件数量
        size_t total_sstable_size;       // SSTable 总大小
    };
    Stats get_stats() const;

    /**
     * @brief 关闭存储引擎，确保所有数据都持久化。
     */
    void close();

    /**
     * @brief 创建全量物理快照 (Checkpoint)。
     *
     * 将当前所有内存数据刷盘，并将底层的 SSTable 文件和元数据
     * 复制到指定的快照目录。
     *
     * @param ssnapshot_tar_path 快照文件路径(输出参数)
     * @return 成功返回 true
     */
    bool create_checkpoint(std::string &output_tar_path, const std::string &extra_meta_data = "");

    /**
     * @brief 从快照恢复数据。
     *
     * 警告：这将覆盖当前的所有数据！
     * 此操作会先关闭引擎，替换数据文件，然后重新加载。
     *
     * @param ssnapshot_tar_path 快照文件路径
     * @return 成功返回 true
     */
    bool restore_from_checkpoint(const std::string &snapshot_tar_path, std::string &out_extra_meta_data);

    /**
     * @brief 删除快照。
     *
     * @param snapshot_path 快照文件路径
     * @return 成功返回 true
     */
    bool remove_snapshot(const std::string &snapshot_path);
    /**
     * @brief 清空并备份当前的数据
     * @return 成功返回true
     */
    bool clear_and_backup_data();
    /**
     * @brief 数据是否为空
     */
    bool empty() const;

private:
    std::string data_dir_;
    std::string sstable_dir_;
    std::string current_wal_filename_;
    // 活跃的 MemTable（接受新写入）
    std::unique_ptr<MemTable> memtable_;

    // Immutable MemTables（等待 Flush 到 SSTable）
    std::map<std::string, std::unique_ptr<MemTable>> immutable_memtables_;
    mutable std::shared_mutex immutable_mutex_; // 保护 immutable_memtables_
    mutable std::shared_mutex write_mutex_;
    // SSTable 管理器
    std::unique_ptr<SSTableManager> sstable_manager_;

    // WAL
    std::unique_ptr<Wal> wal_;
    // Processors
    std::unordered_map<uint8_t, std::shared_ptr<ValueProcessor>> processors_;
    // 配置
    const bool enable_wal_;
    const bool read_only_;
    size_t memtable_size_;
    const size_t skiplist_max_level_;
    const double skiplist_probability_;
    const WALFlushStrategy wal_flush_strategy_;
    std::optional<uint32_t> wal_flush_interval_;
    const uint32_t sstable_merge_threshold_;
    const uint64_t sstable_zero_level_size_;
    const double sstable_level_size_ratio_;
    const SSTableMergeStrategy sstable_merge_strategy_;
    const std::string wal_dir_;
    // 后台线程控制
    std::atomic<bool> background_flush_thread_running_{false};
    std::atomic<bool> closed_{false};
    std::thread flush_thread_;
    std::condition_variable flush_cv_;
    std::mutex flush_mutex_;

    // 快照缓存
    std::atomic<bool> snapshot_cache_valid_{false}; // 快照缓存是否有效
    std::string snapshot_cache_path_;               // 快照缓存路径
    // 是否已经初始化
    static bool is_init_;

    // 静态单例
    static std::unique_ptr<Storage> instance_;
    /**
     * @brief 系统启动时的数据恢复流程。
     * 包括重新加载 SSTable 和重放 WAL。
     */
    void recover();

    /**
     * @brief 启动后台 Flush 线程。
     * 用于异步将 Immutable MemTable 刷盘。
     */
    void start_background_flush_thread();

    /**
     * @brief 停止后台 Flush 线程。
     * 在析构或 Close 时调用。
     */
    void stop_background_flush_thread();

    /**
     * @brief 后台 Flush 线程的主循环。
     */
    void background_flush_task();

    /**
     * @brief 执行具体的 Flush 操作：Immutable MemTable -> SSTable。
     */
    void flush_memtable_to_sstable();

    /**
     * @brief 将当前的 Mutable MemTable 转换为 Immutable MemTable。
     * 并创建一个新的空 MemTable 接收写入。
     */
    void rotate_memtable();

    /**
     * @brief 在所有 Immutable MemTable 中查找 key。
     * 同样遵循从新到旧的顺序。
     */
    std::optional<EValue> get_from_immutable_memtables(const std::string &key) const;

    /**
     * @brief 工厂方法：创建一个配置好的新 MemTable 实例。
     */
    std::unique_ptr<MemTable> create_new_memtable();

    /**
     * @brief 内部写入实现，处理 WAL 和 MemTable 的写入。
     */
    bool write_memtable(const std::string &key, EValue &value);
    /**
     * @brief 获取处理器
     */
    std::shared_ptr<ValueProcessor> get_processor(uint8_t type) const
    {
        auto it = processors_.find(type);
        if (it != processors_.end())
        {
            return it->second;
        }
        return nullptr;
    }

    // 初始化内置命令处理器
    void init_command_handlers();

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
    uint32_t remove(std::vector<std::string> &keys);

    /**
     * @brief 检查 key 是否存在。
     * @param key 要检查的 key
     * @return 如果存在返回 true
     */
    bool contains(const std::string &key) const;

    /**
     * @brief 范围查询 - 获取指定范围内的所有 KV 对。
     * @param start_key 起始 key（包含）
     * @param end_key 结束 key（包含）
     * @return 范围内的 KV 对列表（按 key 排序）
     */
    std::vector<std::pair<std::string, EyaValue>> range(
        const std::string &start_key,
        const std::string &end_key) const;

    /**
     * @brief 设置key的存活时间（从当前时间戳开始）。
     * @param key 要设置过期时间的key
     * @param alive_time 存活时间（秒）
     * @return 设置成功返回true，否则返回false
     */
    void set_expire(const std::string &key, uint64_t alive_time);
    /**
     * @brief 设置key的过期时间戳。
     * @param key 要设置过期时间的key
     * @param expire_time 过期时间戳（秒）
     * @return 设置成功返回true，否则返回false
     */
    void set_key_expire(const std::string &key, uint64_t expire_time);
    /**
     * @brief 根据key从最新数据中获取
     */
    bool get_from_latest(const std::string &key, std::optional<EValue> &value) const;

    /**
     * @brief 根据key从旧数据中获取
     */
    bool get_from_old(const std::string &key, std::optional<EValue> &value) const;

    /**
     * @brief 获取所有的keys
     */
    std::set<std::string> keys(const std::string &pattern) const;

    // 设置友元类
    friend class StringProcessor;
    friend class SetProcessor;
    friend class ZSetProcessor;
    friend class DequeProcessor;
    friend class HashProcessor;
};

#endif // STORAGE_H
