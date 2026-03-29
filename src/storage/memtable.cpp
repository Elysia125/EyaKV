#include <shared_mutex>
#include <algorithm>
#include <numeric>
#include <queue>
#include "logger/logger.h"
#include "storage/memtable.h"

// MemTable 实现
MemTable::MemTable(const size_t &memtable_size,
                   const size_t &skiplist_max_level,
                   const double &skiplist_probability) : memtable_size_(memtable_size * 1024),
                                                         size_(0)
{
    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        tables_.push_back(std::make_unique<SkipList<std::string, EValue>>(
            skiplist_max_level,
            skiplist_probability,
            std::nullopt,
            calculateStringSize,
            estimateEValueSize));

        // 初始化分片锁
        shard_locks_.push_back(std::make_unique<std::shared_mutex>());

        // 假设原本 1000000 是总期望元素数量，分片后每个分片期望数量为 total / shards
        bloom_filters_.push_back(std::make_unique<BloomFilter>(1000000 / k_num_shards_));
        bloom_locks_.push_back(std::make_unique<std::shared_mutex>());
    }
}

size_t MemTable::get_shard_index(const std::string &key) const
{
    // Range(范围) 分片策略
    // 按照字符串第一个字节 (0x00 ~ 0xFF) 平均划分到各个分片中。
    // 这样能保证: 分片 0 里的所有 Key 必然小于 分片 1 里的所有 Key。

    if (key.empty())
    {
        return 0;
    }

    // 获取第一个字符对应的无符号数值 (0 - 255)
    unsigned char first_byte = static_cast<unsigned char>(key[0]);

    // 计算分配的分片: 256 / 16 = 16，每个分片负责 16 个前缀
    size_t idx = first_byte / (256 / k_num_shards_);

    // 防止边界异常，严格限制在合法分片范围内
    return std::min(idx, k_num_shards_ - 1);
}

void MemTable::put(const std::string &key, const EValue &value)
{
    LOG_DEBUG("MemTable::put key={}", key.c_str());
    if (should_flush())
    {
        throw std::overflow_error("MemTable size exceeds limit");
    }

    size_t idx = get_shard_index(key);

    // 获取写锁（独占锁）
    std::unique_lock<std::shared_mutex> shard_lock(*shard_locks_[idx]);

    // 获取插入前的分片大小
    size_t old_shard_size = tables_[idx]->size();

    // 插入跳表
    tables_[idx]->insert(key, value);

    // 获取插入后的分片大小，判断是否为新增
    size_t new_shard_size = tables_[idx]->size();
    if (new_shard_size > old_shard_size)
    {
        // 新增元素，更新总数
        size_.fetch_add(1, std::memory_order_relaxed);
        // 更新 BloomFilter
        std::unique_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        bloom_filters_[idx]->add(key);
    }
}

std::optional<EValue> MemTable::get(const std::string &key) const
{
    size_t idx = get_shard_index(key);

    if (!bloom_filters_[idx]->may_contain(key))
    {
        LOG_DEBUG("MemTable::get key={} BloomFilter miss", key.c_str());
        return std::nullopt;
    }

    try
    {
        auto result = tables_[idx]->get(key);
        LOG_DEBUG("MemTable::get key={} found", key.c_str());
        return result;
    }
    catch (const std::exception &e)
    {
        LOG_WARN("MemTable::get key={} exception: {}", key.c_str(), e.what());
        return std::nullopt;
    }
}

bool MemTable::remove(const std::string &key)
{
    LOG_DEBUG("MemTable::remove key={} (Logical Delete)", key.c_str());
    size_t idx = get_shard_index(key);

    std::unique_lock<std::shared_mutex> shard_lock(*shard_locks_[idx]);

    try
    {
        tables_[idx]->handle_value(key, [](EValue &val) -> EValue &
                                   {
            val.deleted = true;
            return val; });
        return true;
    }
    catch (const std::out_of_range &)
    {
        EValue tombstone;
        tombstone.deleted = true;

        tables_[idx]->insert(key, tombstone);

        size_.fetch_add(1, std::memory_order_relaxed);

        std::unique_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        bloom_filters_[idx]->add(key);

        return false;
    }
}

size_t MemTable::size() const
{
    // 直接返回缓存的大小，O(1) 复杂度
    return size_.load(std::memory_order_relaxed);
}

size_t MemTable::memory_usage() const
{
    size_t usage = sizeof(MemTable);
    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        // 获取读锁
        std::shared_lock<std::shared_mutex> shard_lock(*shard_locks_[i]);

        usage += tables_[i]->memory_usage();

        // BloomFilter锁保持独立
        std::shared_lock<std::shared_mutex> lock(*bloom_locks_[i]);
        usage += bloom_filters_[i]->size();
    }
    return usage;
}

size_t MemTable::memory_limit() const
{
    return memtable_size_;
}

bool MemTable::should_flush() const
{
    return memtable_size_ != 0 && memory_usage() >= memtable_size_;
}

void MemTable::clear()
{
    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        // 获取写锁（独占锁）
        std::unique_lock<std::shared_mutex> shard_lock(*shard_locks_[i]);

        tables_[i]->clear();

        // 重置 BloomFilter（已有bloom_locks_保护）
        std::unique_lock<std::shared_mutex> lock(*bloom_locks_[i]);
        bloom_filters_[i] = std::make_unique<BloomFilter>(1000000 / k_num_shards_);
    }
    // 重置总数
    size_.store(0, std::memory_order_relaxed);
}

std::vector<std::pair<std::string, EValue>> MemTable::get_all_entries() const
{
    // 对所有分片获取读锁，保证收集数据的 Point-in-time 视图一致性
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    locks.reserve(k_num_shards_);
    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        locks.emplace_back(*shard_locks_[i]);
    }

    std::vector<std::pair<std::string, EValue>> result;
    // 预分配内存，避免多次扩容
    result.reserve(size());

    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        auto shard_entries = tables_[i]->get_all_entries();
        // 如果想更高效，可以使用 std::make_move_iterator
        result.insert(result.end(),
                      std::make_move_iterator(shard_entries.begin()),
                      std::make_move_iterator(shard_entries.end()));
    }

    return result;
}

void MemTable::for_each(const std::function<void(const std::string &, const EValue &)> &callback) const
{
    // 为了保证 Key 有序调用，必须先获取所有并排序
    auto entries = get_all_entries();
    for (const auto &entry : entries)
    {
        callback(entry.first, entry.second);
    }
}

EValue MemTable::handle_value(const std::string &key, std::function<EValue &(EValue &)> value_handle)
{
    size_t idx = get_shard_index(key);

    // 检查 BloomFilter
    {
        std::shared_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        if (!bloom_filters_[idx]->may_contain(key))
        {
            throw std::out_of_range("Key not found");
        }
    }

    // 获取写锁（独占锁）
    std::unique_lock<std::shared_mutex> shard_lock(*shard_locks_[idx]);

    return tables_[idx]->handle_value(key, value_handle);
}

void MemTable::cancel_size_limit()
{
    memtable_size_ = 0;
}

void MemTable::set_size_limit(size_t size)
{
    memtable_size_ = size;
}