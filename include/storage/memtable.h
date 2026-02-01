#ifndef TINYKV_STORAGE_MEMTABLE_H_
#define TINYKV_STORAGE_MEMTABLE_H_

#include <string>
#include <map>
#include <shared_mutex>
#include <optional>
#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include "common/ds/skip_list.h"
#include "storage/node.h"
#include "common/util/string_utils.h"
#include "common/ds/bloom_filter.h"
/**
 * @brief MemTable（内存表）负责在内存中存储 Key-Value 数据。
 *
 * 它是存储引擎的第一层，所有写入操作首先写入 MemTable。
 * 为了支持并发访问，内部使用了读写锁 (std::shared_mutex)。
 * 使用跳表保持 Key 有序，方便后续 Flush 到 SSTable。
 *
 * 内存大小跟踪：
 * - 每次插入/更新/删除都会更新当前使用的内存大小估算
 * - 当内存使用超过阈值时，触发 Flush 到 SSTable
 *
 */
class MemTable
{
public:
    /**
     * @brief 构造函数，创建一个新的 MemTable 实例。
     *
     * @param memtable_size MemTable 的大小限制（单位：KB），超过此大小会触发 Flush
     * @param skiplist_max_level 跳表的最大层级，影响查询效率，推荐值为 16
     * @param skiplist_probability 跳表的提升概率，推荐值为 0.5
     */
    MemTable(const size_t &memtable_size,
             const size_t &skiplist_max_level,
             const double &skiplist_probability);

    /**
     * @brief 析构函数，释放 MemTable 占用的资源。
     */
    ~MemTable() = default;

    // 禁止拷贝和赋值，避免意外的开销和锁问题
    MemTable(const MemTable &) = delete;
    MemTable &operator=(const MemTable &) = delete;
    // 允许移动
    MemTable(MemTable &&) = default;
    MemTable &operator=(MemTable &&) = default;

    /**
     * @brief 插入或更新一个 Key-Value 对。
     *
     * 如果 Key 已存在，则更新其对应的 Value；如果不存在，则插入新的 KV 对。
     * 此操作会获取写锁（独占锁），保证线程安全。
     * 如果 MemTable 已满，会抛出 std::overflow_error 异常。
     *
     * @param key 要插入或更新的键
     * @param value 对应的值
     * @throw std::overflow_error 当 MemTable 大小超过限制时抛出
     */
    void put(const std::string &key, const EValue &value);

    /**
     * @brief 获取指定 Key 对应的值。
     *
     * 此操作会获取读锁（共享锁），允许多个读操作并发执行。
     *
     * @param key 要查询的键
     * @return std::optional<EValue> 如果 Key 存在，返回对应的 Value；否则返回 std::nullopt
     */
    std::optional<EValue> get(const std::string &key) const;

    /**
     * @brief 标记删除指定的 Key。
     *
     * 在 LSM-tree 架构中，删除操作不会立即从内存中移除数据，
     * 而是将其标记为已删除（tombstone）。真正的删除发生在 Compaction 过程中。
     * 此操作会获取写锁（独占锁），保证线程安全。
     *
     * @param key 要删除的键
     * @return true 如果成功标记删除
     * @return false 如果 Key 不存在
     */
    bool remove(const std::string &key);

    /**
     * @brief 获取当前 MemTable 中存储的元素数量。
     *
     * 此操作会获取读锁（共享锁）。
     *
     * @return size_t 当前存储的 KV 对数量
     */
    size_t size() const;

    /**
     * @brief 获取当前 MemTable 估算使用的内存大小。
     *
     * 包括 MemTable 对象本身的大小以及底层跳表占用的内存。
     * 此操作会获取读锁（共享锁）以保证线程安全。
     *
     * @return size_t 估算的内存使用量（字节）
     */
    size_t memory_usage() const;

    /**
     * @brief 获取 MemTable 的内存大小限制。
     *
     * @return size_t 内存大小限制（字节）
     */
    size_t memory_limit() const;

    /**
     * @brief 判断 MemTable 是否需要 Flush 到磁盘。
     *
     * 当 memory_usage() >= memory_limit() 时返回 true。
     *
     * @return true 如果需要 Flush
     * @return false 如果不需要 Flush
     */
    bool should_flush() const;

    /**
     * @brief 清空 MemTable 中的所有数据。
     *
     * 此操作会获取写锁（独占锁），主要用于测试或重置场景。
     */
    void clear();

    /**
     * @brief 获取所有 Key-Value 对，用于 Flush 到 SSTable。
     *
     * 返回的数据按 Key 升序排列，适合直接写入 SSTable。
     * 此操作会获取读锁（共享锁）。
     *
     * @return std::vector<std::pair<std::string, EValue>> 包含所有 KV 对的向量，按 Key 升序排列
     */
    std::vector<std::pair<std::string, EValue>> get_all_entries() const;

    /**
     * @brief 遍历所有 Key-Value 对，按 Key 升序调用回调函数。
     *
     * 此操作会获取读锁（共享锁），在遍历过程中数据不会被修改。
     *
     * @param callback 回调函数，接受 (const std::string&, const EValue&) 参数
     */
    void for_each(const std::function<void(const std::string &, const EValue &)> &callback) const;

    /**
     * @brief 对指定 Key 对应的 Value 进行原子操作。
     *
     * 允许在持有锁的情况下修改 Value，保证操作的原子性。
     * 此操作会获取写锁（独占锁）。
     *
     * @param key 要操作的键
     * @param value_handle 操作函数，接受 EValue& 参数，返回修改后的 EValue&
     * @return EValue 操作之前的 Value 值
     * @throw std::out_of_range 当 Key 不存在或已过期/被标记删除时抛出
     * @note 如果 Key 已过期，会自动标记为删除状态
     */
    EValue handle_value(const std::string &key, std::function<EValue &(EValue &)> value_handle);

    /**
     * @brief 取消内存大小限制。
     */
    void cancel_size_limit();
    /**
     * @brief 设置内存大小限制。
     * @param size 限制的内存大小（字节数）
     */
    void set_size_limit(size_t size);
    
private:
    size_t memtable_size_; /// MemTable 大小限制（字节数）

    static constexpr size_t k_num_shards_ = 16;  /// 分片数量（必须为 2 的幂次）
    std::vector<std::unique_ptr<SkipList<std::string, EValue>>> tables_; /// 分片存储的跳表数组
    size_t get_shard_index(const std::string &key) const; /// 计算 Key 对应的分片索引

    std::atomic<size_t> size_{0};  /// 当前存储的元素总数（原子变量，线程安全）

    std::vector<std::unique_ptr<BloomFilter>> bloom_filters_; /// 分片布隆过滤器
    mutable std::vector<std::unique_ptr<std::shared_mutex>> bloom_locks_; /// 分片布隆过滤器锁
};
#endif