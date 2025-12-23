#include "storage/memtable.h"
#include <mutex>
#include "logger/logger.h"

MemTable::MemTable(const size_t &memtable_size,
                   const size_t &skiplist_max_level,
                   const double &skiplist_probability,
                   const size_t &skiplist_max_node_count) : memtable_size_(memtable_size * 1024),
                                                            table_(skiplist_max_level,
                                                                   skiplist_probability,
                                                                   skiplist_max_node_count)
{
}

void MemTable::put(const std::string &key, const std::string &value)
{
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.insert(key, value);
}

std::optional<std::string> MemTable::get(const std::string &key) const
{
    // 获取读锁 (共享锁)
    std::shared_lock<std::shared_mutex> lock(mutex_);
    try
    {
        return table_.get(key);
    }
    catch (const std::exception &e)
    {
        LOG_WARN("MemTable get error: {}", e.what());
        return std::nullopt;
    }
}

void MemTable::remove(const std::string &key)
{
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.remove(key);
}

size_t MemTable::size() const
{
    // 获取读锁
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

void MemTable::clear()
{
    // 获取写锁
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
}