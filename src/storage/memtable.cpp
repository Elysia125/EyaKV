#include "storage/memtable.h"
#include <mutex>
#include "logger/logger.h"
#include "common/common.h"

// ==================== 内存大小估算辅助函数 ====================

/**
 * @brief 估算 EyaValue 占用的内存大小
 */
inline size_t estimateEyaValueSize(const EyaValue& value) {
    return std::visit([](auto&& arg) -> size_t {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return arg.size() + sizeof(std::string);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            size_t total = sizeof(std::vector<std::string>);
            for (const auto& s : arg) {
                total += s.size() + sizeof(std::string);
            }
            return total;
        } else if constexpr (std::is_same_v<T, std::unordered_set<std::string>>) {
            size_t total = sizeof(std::unordered_set<std::string>);
            for (const auto& s : arg) {
                total += s.size() + sizeof(std::string) + 32; // 哈希表节点开销
            }
            return total;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::string>>) {
            size_t total = sizeof(std::unordered_map<std::string, std::string>);
            for (const auto& [k, v] : arg) {
                total += k.size() + v.size() + sizeof(std::string) * 2 + 32;
            }
            return total;
        } else if constexpr (std::is_same_v<T, ZSet>) {
            // ZSet 的大小估算较为复杂，使用一个粗略值
            return sizeof(ZSet) + arg.zcard() * 128;
        } else {
            return sizeof(T);
        }
    }, value);
}

// ==================== MemTable 实现 ====================

template <typename K, typename V>
MemTable<K, V>::MemTable(const size_t &memtable_size,
                         const size_t &skiplist_max_level,
                         const double &skiplist_probability,
                         const size_t &skiplist_max_node_count) : memtable_size_(memtable_size * 1024),
                                                                  current_size_(0),
                                                                  table_(skiplist_max_level,
                                                                         skiplist_probability,
                                                                         skiplist_max_node_count)
{
}

template <typename K, typename V>
void MemTable<K, V>::put(const K &key, const V &value)
{
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 检查是否是更新操作
    try {
        V old_value = table_.get(key);
        // 更新：减去旧值大小，加上新值大小
        size_t old_size = estimateEntrySize(key, old_value);
        size_t new_size = estimateEntrySize(key, value);
        if (new_size > old_size) {
            current_size_.fetch_add(new_size - old_size, std::memory_order_relaxed);
        } else {
            current_size_.fetch_sub(old_size - new_size, std::memory_order_relaxed);
        }
    } catch (const std::out_of_range&) {
        // 插入新条目
        current_size_.fetch_add(estimateEntrySize(key, value), std::memory_order_relaxed);
    }
    
    table_.insert(key, value);
}

template <typename K, typename V>
std::optional<V> MemTable<K, V>::get(const K &key) const
{
    // 获取读锁 (共享锁)
    std::shared_lock<std::shared_mutex> lock(mutex_);
    try
    {
        return table_.get(key);
    }
    catch (const std::exception &e)
    {
        return std::nullopt;
    }
}

template <typename K, typename V>
bool MemTable<K, V>::remove(const K &key)
{
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    try {
        V old_value = table_.get(key);
        size_t entry_size = estimateEntrySize(key, old_value);
        if (table_.remove(key)) {
            current_size_.fetch_sub(entry_size, std::memory_order_relaxed);
            return true;
        }
    } catch (const std::out_of_range&) {
        // key 不存在
    }
    return false;
}

template <typename K, typename V>
size_t MemTable<K, V>::size() const
{
    // 获取读锁
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

template <typename K, typename V>
size_t MemTable<K, V>::memoryUsage() const
{
    return current_size_.load(std::memory_order_relaxed);
}

template <typename K, typename V>
size_t MemTable<K, V>::memoryLimit() const
{
    return memtable_size_;
}

template <typename K, typename V>
bool MemTable<K, V>::shouldFlush() const
{
    return current_size_.load(std::memory_order_relaxed) >= memtable_size_;
}

template <typename K, typename V>
void MemTable<K, V>::clear()
{
    // 获取写锁
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
    current_size_.store(0, std::memory_order_relaxed);
}

template <typename K, typename V>
std::vector<std::pair<K, V>> MemTable<K, V>::getAllEntries() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::pair<K, V>> entries;
    entries.reserve(table_.size());
    
    // 使用跳表的范围查询功能获取所有条目
    // 由于跳表按 key 排序，返回的数据自动是有序的
    auto all = table_.range_by_rank(0, table_.size() - 1);
    for (auto& [key, value] : all) {
        entries.emplace_back(std::move(key), std::move(value));
    }
    return entries;
}

template <typename K, typename V>
void MemTable<K, V>::forEach(const std::function<void(const K &, const V &)> &callback) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto all = table_.range_by_rank(0, table_.size() - 1);
    for (const auto& [key, value] : all) {
        callback(key, value);
    }
}

// ==================== 模板特化的 estimateEntrySize ====================

template <>
size_t MemTable<std::string, EyaValue>::estimateEntrySize(const std::string &key, const EyaValue &value)
{
    // 跳表节点开销（指针数组等）估算为 128 字节
    constexpr size_t SKIPLIST_NODE_OVERHEAD = 128;
    
    size_t key_size = key.size() + sizeof(std::string);
    size_t value_size = estimateEyaValueSize(value);
    
    return key_size + value_size + SKIPLIST_NODE_OVERHEAD;
}

// 通用模板实现
template <typename K, typename V>
size_t MemTable<K, V>::estimateEntrySize(const K &key, const V &value)
{
    // 对于通用类型，使用 sizeof 估算
    constexpr size_t SKIPLIST_NODE_OVERHEAD = 128;
    return sizeof(K) + sizeof(V) + SKIPLIST_NODE_OVERHEAD;
}

// 显式实例化模板
template class MemTable<std::string, EyaValue>;
template class MemTable<std::string, std::string>;