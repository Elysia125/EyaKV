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
 * @brief MemTable 负责在内存中存储 Key-Value 数据。
 *
 * 它是存储引擎的第一层，所有写入操作首先写入 MemTable。
 * 为了支持并发访问，内部使用了读写锁 (std::shared_mutex)。
 * 使用跳表保持 Key 有序，方便后续 Flush 到 SSTable。
 * 
 * 内存大小跟踪：
 * - 每次插入/更新/删除都会更新当前使用的内存大小估算
 * - 当内存使用超过阈值时，应该触发 Flush 到 SSTable
 */
template <typename K, typename V>
class MemTable
{
public:
    MemTable(const size_t &memtable_size,
             const size_t &skiplist_max_level,
             const double &skiplist_probability,
             const size_t &skiplist_max_node_count);
    ~MemTable() = default;

    // 禁止拷贝和赋值，避免意外的开销和锁问题
    MemTable(const MemTable &) = delete;
    MemTable &operator=(const MemTable &) = delete;
    // 允许移动
    MemTable(MemTable &&) = default;
    MemTable &operator=(MemTable &&) = default;

    /**
     * @brief 插入或更新一个 Key-Value 对。
     * @param key 键
     * @param value 值
     */
    void put(const K &key, const V &value);

    /**
     * @brief 获取指定 Key 的值。
     * @param key 键
     * @return 如果 Key 存在，返回对应的 Value；否则返回 std::nullopt。
     */
    std::optional<V> get(const K &key) const;

    /**
     * @brief 删除指定 Key。
     * @param key 键
     * @return 如果删除成功返回 true，如果 Key 不存在返回 false。
     */
    bool remove(const K &key);

    /**
     * @brief 获取当前 MemTable 中包含的元素数量。
     */
    size_t size() const;

    /**
     * @brief 获取当前 MemTable 使用的内存大小估算（字节）。
     */
    size_t memoryUsage() const;

    /**
     * @brief 获取 MemTable 的大小限制（字节）。
     */
    size_t memoryLimit() const;

    /**
     * @brief 判断 MemTable 是否需要 Flush 到磁盘。
     * @return 如果当前内存使用超过阈值，返回 true。
     */
    bool shouldFlush() const;

    /**
     * @brief 清空 MemTable (主要用于测试或重置)。
     */
    void clear();

    /**
     * @brief 获取所有 Key-Value 对用于 Flush 到 SSTable。
     * 返回的数据按 Key 升序排列。
     * @return 包含所有 KV 对的 vector
     */
    std::vector<std::pair<K, V>> getAllEntries() const;

    /**
     * @brief 遍历所有 Key-Value 对，按 Key 升序调用回调函数。
     * @param callback 回调函数，参数为 (key, value)
     */
    void forEach(const std::function<void(const K &, const V &)> &callback) const;

private:
    size_t memtable_size_;              // MemTable 大小限制（字节数）
    std::atomic<size_t> current_size_{0}; // 当前使用的内存大小估算

    // 核心数据结构，使用跳表
    SkipList<K, V> table_;

    // 读写锁，保护 table_ 的并发访问
    mutable std::shared_mutex mutex_;

    /**
     * @brief 估算一个 Key-Value 对占用的内存大小。
     * 这是一个粗略的估算，包括 key 和 value 的大小以及跳表节点的开销。
     */
    static size_t estimateEntrySize(const K &key, const V &value);
};
#endif