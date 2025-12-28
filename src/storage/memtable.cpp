#include "storage/memtable.h"
#include <mutex>
#include "logger/logger.h"
#include "common/common.h"

// ==================== MemTable 实现 ====================

template <typename K, typename V>
MemTable<K, V>::MemTable(const size_t &memtable_size,
                         const size_t &skiplist_max_level,
                         const double &skiplist_probability,
                         const size_t &skiplist_max_node_count,
                         std::optional<int (*)(const K &, const K &)> compare_func = std::nullopt,
                         std::optional<size_t (*)(const K &)> calculate_key_size_func = std::nullopt,
                         std::optional<size_t (*)(const V &)> calculate_value_size_func = std::nullopt) : memtable_size_(memtable_size * 1024),
                                                                                                          current_size_(0),
                                                                                                          table_(skiplist_max_level,
                                                                                                                 skiplist_probability,
                                                                                                                 skiplist_max_node_count,
                                                                                                                 compare_func,
                                                                                                                 calculate_key_size_func,
                                                                                                                 calculate_value_size_func)
{
}

template <typename K, typename V>
void MemTable<K, V>::put(const K &key, const V &value)
{
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (should_flush())
    {
        throw std::overflow_error("MemTable size exceeds limit");
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
    return table_.handle_value(key, [](V &value) -> V &
                               {
        value.is_deleted = true;
        return value; });
}
template <typename K, typename V>
size_t MemTable<K, V>::size() const
{
    // 获取读锁
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

template <typename K, typename V>
size_t MemTable<K, V>::memory_usage() const
{
    return sizeof(MemTable<K, V>) + table_.memory_usage();
}
template <typename K, typename V>
size_t MemTable<K, V>::memory_limit() const
{
    return memtable_size_;
}

template <typename K, typename V>
bool MemTable<K, V>::should_flush() const
{
    return memory_usage() >= memtable_size_;
}

template <typename K, typename V>
void MemTable<K, V>::clear()
{
    // 获取写锁
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
}

template <typename K, typename V>
std::vector<std::pair<K, V>> MemTable<K, V>::get_all_entries() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.get_all_entries();
}
template <typename K, typename V>
void MemTable<K, V>::for_each(const std::function<void(const K &, const V &)> &callback) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    table_.for_each(callback);
}

template <typename K, typename V>
V MemTable<K, V>::handle_value(const K &key, std::function<V &(V &)> value_handle)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return table_.handle_value(key, value_handle);
}