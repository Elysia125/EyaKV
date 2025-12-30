#ifndef TINYKV_STORAGE_MEMTABLE_H_
#define TINYKV_STORAGE_MEMTABLE_H_

#include <string>
#include <map>
#include <shared_mutex>
#include <optional>
#include <functional>
#include <vector>
#include "common/skip_list.h"

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
 * @tparam K 键的类型
 * @tparam V 值的类型
 */
template <typename K, typename V>
class MemTable
{
public:
    /**
     * @brief 构造函数，创建一个新的 MemTable 实例。
     *
     * @param memtable_size MemTable 的大小限制（单位：KB），超过此大小会触发 Flush
     * @param skiplist_max_level 跳表的最大层级，影响查询效率，推荐值为 16
     * @param skiplist_probability 跳表的提升概率，推荐值为 0.5
     * @param skiplist_max_node_count 跳表的最大节点数量限制
     * @param compare_func 可选的自定义键比较函数，用于键的排序
     * @param calculate_key_size_func 可选的自定义键大小计算函数，用于内存估算
     * @param calculate_value_size_func 可选的自定义值大小计算函数，用于内存估算
     */
    MemTable(const size_t &memtable_size,
             const size_t &skiplist_max_level,
             const double &skiplist_probability,
             const size_t &skiplist_max_node_count,
             std::optional<int (*)(const K &, const K &)> compare_func = std::nullopt,
             std::optional<size_t (*)(const K &)> calculate_key_size_func = std::nullopt,
             std::optional<size_t (*)(const V &)> calculate_value_size_func = std::nullopt);

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
    void put(const K &key, const V &value);

    /**
     * @brief 获取指定 Key 对应的值。
     *
     * 此操作会获取读锁（共享锁），允许多个读操作并发执行。
     *
     * @param key 要查询的键
     * @return std::optional<V> 如果 Key 存在，返回对应的 Value；否则返回 std::nullopt
     */
    std::optional<V> get(const K &key) const;

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
    bool remove(const K &key);

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
     * @return std::vector<std::pair<K, V>> 包含所有 KV 对的向量，按 Key 升序排列
     */
    std::vector<std::pair<K, V>> get_all_entries() const;

    /**
     * @brief 遍历所有 Key-Value 对，按 Key 升序调用回调函数。
     *
     * 此操作会获取读锁（共享锁），在遍历过程中数据不会被修改。
     *
     * @param callback 回调函数，接受 (const K&, const V&) 参数
     */
    void for_each(const std::function<void(const K &, const V &)> &callback) const;

    /**
     * @brief 对指定 Key 对应的 Value 进行原子操作。
     *
     * 允许在持有锁的情况下修改 Value，保证操作的原子性。
     * 此操作会获取写锁（独占锁）。
     *
     * @param key 要操作的键
     * @param value_handle 操作函数，接受 V& 参数，返回修改后的 V&
     * @return V 操作之前的 Value 值
     * @throw std::out_of_range 当 Key 不存在或已过期/被标记删除时抛出
     * @note 如果 Key 已过期，会自动标记为删除状态
     */
    V handle_value(const K &key, std::function<V &(V &)> value_handle);

    void set_memory_limit(const size_t &memtable_size);

    void set_skiplist_max_node_count(const size_t &skiplist_max_node_count);

private:
    size_t memtable_size_; ///< MemTable 大小限制（字节数）

    SkipList<K, V> table_; ///< 核心数据结构，使用跳表存储有序的 KV 对

    mutable std::shared_mutex mutex_; ///< 读写锁，保护 table_ 的并发访问
};
#endif