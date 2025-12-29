#ifndef STORAGE_H_
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
#include <condition_variable>
#include "common/common.h"
#include "storage/processors/processor.h"

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
                     const std::optional<unsigned int> &wal_flush_interval = 1000,
                     const WALFlushStrategy &wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD,
                     const size_t &memtable_size = 1024 * 1024 * 1024,
                     const size_t &skiplist_max_level = 16,
                     const double &skiplist_probability = 0.5,
                     const size_t &skiplist_max_node_count = 10000000,
                     const SSTableMergeStrategy &sstable_merge_strategy = SSTableMergeStrategy::SIZE_TIERED_COMPACTION,
                     const unsigned int &sstable_merge_threshold = 5,
                     const unsigned int &sstable_zero_level_size = 10,
                     const double &sstable_level_size_ratio = 10.0);
    ~Storage();

    // 禁止拷贝
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    // 允许移动
    Storage(Storage &&) = default;
    Storage &operator=(Storage &&) = default;

    /**
     * @brief 读取数据。
     *
     * 流程（按优先级）：
     * 1. 查询 MemTable
     * 2. 查询 Immutable MemTables
     * 3. 查询 SSTable
     */
    std::optional<EyaValue> get(const std::string &key) const;

    template <typename... Args>
    Result excute(uint8_t type, Args &&...args);
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

private:
    std::string data_dir_;
    std::string sstable_dir_;
    std::string current_wal_filename_;
    // 活跃的 MemTable（接受新写入）
    std::unique_ptr<MemTable<std::string, EValue>> memtable_;

    // Immutable MemTables（等待 Flush 到 SSTable）
    std::map<std::string, std::unique_ptr<MemTable<std::string, EValue>>> immutable_memtables_;
    mutable std::shared_mutex immutable_mutex_; // 保护 immutable_memtables_

    // SSTable 管理器
    std::unique_ptr<SSTableManager> sstable_manager_;

    // WAL
    std::unique_ptr<Wal> wal_;
    // Processors
    std::unordered_map<uint8_t, std::shared_ptr<ValueProcessor>> processors_;
    // 配置
    bool enable_wal_;
    bool read_only_;
    size_t memtable_size_;
    size_t skiplist_max_level_;
    double skiplist_probability_;
    size_t skiplist_max_node_count_;
    WALFlushStrategy wal_flush_strategy_;
    std::optional<unsigned int> wal_flush_interval_;

    // 后台线程控制
    std::atomic<bool> background_flush_thread_running_{false};
    std::atomic<bool> closed_{false};
    std::thread flush_thread_;
    std::condition_variable flush_cv_;
    std::mutex flush_mutex_;

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
    std::unique_ptr<MemTable<std::string, EValue>> create_new_memtable();

    /**
     * @brief 内部写入实现，处理 WAL 和 MemTable 的写入。
     */
    bool write_memtable(const std::string &key, EValue &value);

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
    template <typename... Args>
    uint32_t remove(Args &&...args);

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
     * @brief 设置key的过期时间。
     * @param key 要设置过期时间的key
     * @param alive_time 过期时间（秒）
     * @return 设置成功返回true，否则返回false
     */
    void set_expire(const std::string &key, uint64_t alive_time);

    void set_key_expire(const std::string &key, uint64_t expire_time);
};

template <typename... Args>
uint32_t remove(Args &&...args)
{
    if (read_only_)
    {
        LOG_WARN("Storage: remove failed, read only mode");
        throw std::runtime_error("Remove key failed, read only mode");
    }
    if (sizeof...(Args) == 0)
    {
        throw std::runtime_error("Remove key failed, missing key");
    }
    uint32_t count = 0;
    if (enable_wal_ && wal_)
    {
        (
            [&](auto &&key)
            {
                if (!wal_->append_log(LogType::kRemove, std::forward<decltype(key)>(key), ""))
                {
                    LOG_ERROR("Storage: remove key %s failed, append log failed",
                              std::forward<decltype(key)>(key).c_str());
                    return;
                }
                memtable_->handle_value(key, remove_evalue);
                ++count;
            }(std::forward<Args>(args)),
            ...);
    }
    else
    {
        (
            [&](auto &&key)
            {
                memtable_->handle_value(key, remove_evalue);
                ++count;
            }(std::forward<Args>(args)),
            ...);
    }
    return count;
}

template <typename... Args>
Result Storage::excute(uint8_t type, Args &&...args)
{
    try
    {
        if (type == LogType::kRemove)
        {
            if (sizeof...(Args) == 0)
            {
                return Result.error("missing key");
            }
            return Result.success(remove(std::forward<Args>(args)...));
        }
        else if (type == LogType::kExists)
        {
            if (sizeof...(Args) == 0)
            {
                return Result.error("missing key");
            }
            else if (sizeof...(Args) > 1)
            {
                return Result.error("too many arguments");
            }
            return Result.success(contains(std::forward<Args>(args)...) ? "1" : "0");
        }
        else if (type == LogType::kRange)
        {
            if (sizeof...(Args) <= 1)
            {
                return Result.error("missing key");
            }
            if (sizeof...(Args) > 2)
            {
                return Result.error("too many arguments");
            }
            return Result.success(range(std::forward<Args>(args)...));
        }
        else if (type == LogType::kExpire)
        {
            if (sizeof...(Args) <= 1)
            {
                return Result.error("missing key");
            }
            if (sizeof...(Args) > 2)
            {
                return Result.error("too many arguments");
            }
            auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
            std::string key = std::get<0>(tup);
            uint64_t expire_time = std::stoull(std::get<1>(tup));
            return Result.success(set_expire(key, expire_time));
        }
        else if (type == LogType::kGet)
        {
            if (sizeof...(Args) == 0)
            {
                return Result.error("missing key");
            }
            if (sizeof...(Args) > 1)
            {
                return Result.error("too many arguments");
            }
            auto value = get(std::forward<Args>(args)...);
            EData data = value == std::nullopt ? std::monostate(), value.value();
            return Result.success(data);
        }
        else
        {
            auto processor = get_processor(type);
            if (processor)
            {
                std::vector<std::string> vec = {std::forward<Args>(args)...};
                if (enable_wal_ && wal_)
                {
                    return processor->excute(memtable_.get(), wal_.get(), type, vec);
                }
                else
                {
                    return processor->excute(memtable_.get(), nullptr, type, vec);
                }
            }
            return Result.error("unknown command");
        }
    }
    catch (const std::runtime_error &e)
    {
        return Result.error(e.what());
    }
    catch (const std::exception &e)
    {
        return Result.error("unknown error");
    }
}

#endif // STORAGE_H
