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

        // 假设原本 1000000 是总期望元素数量，分片后每个分片期望数量为 total / shards
        bloom_filters_.push_back(std::make_unique<BloomFilter>(1000000 / k_num_shards_));
        bloom_locks_.push_back(std::make_unique<std::shared_mutex>());
    }
}

size_t MemTable::get_shard_index(const std::string &key) const
{
    // 使用位与操作替代取模，效率更高（要求 k_num_shards_ 为 2 的幂次）
    return std::hash<std::string>{}(key) & (k_num_shards_ - 1);
}

void MemTable::put(const std::string &key, const EValue &value)
{
    LOG_DEBUG("MemTable::put key=%s", key.c_str());
    if (should_flush())
    {
        throw std::overflow_error("MemTable size exceeds limit");
    }

    size_t idx = get_shard_index(key);
    // 获取插入前的分片大小
    size_t old_shard_size = tables_[idx]->size();

    // 插入跳表 (SkipList 内部有锁)
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

    // 检查 BloomFilter
    {
        std::shared_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        if (!bloom_filters_[idx]->may_contain(key))
        {
            LOG_DEBUG("MemTable::get key=%s BloomFilter miss", key.c_str());
            return std::nullopt;
        }
    }

    try
    {
        auto result = tables_[idx]->get(key);
        LOG_DEBUG("MemTable::get key=%s found", key.c_str());
        return result;
    }
    catch (const std::exception &e)
    {
        LOG_WARN("MemTable::get key=%s exception: %s", key.c_str(), e.what());
        return std::nullopt;
    }
}

bool MemTable::remove(const std::string &key)
{
    LOG_DEBUG("MemTable::remove key=%s", key.c_str());
    size_t idx = get_shard_index(key);
    {
        std::shared_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        if (!bloom_filters_[idx]->may_contain(key))
        {
            return false;
        }
    }
    bool removed = tables_[idx]->remove(key);
    if (removed)
    {
        // 删除成功，更新总数
        size_.fetch_sub(1, std::memory_order_relaxed);
    }
    return removed;
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
        usage += tables_[i]->memory_usage();
        {
            std::shared_lock<std::shared_mutex> lock(*bloom_locks_[i]);
            usage += bloom_filters_[i]->size();
        }
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
        tables_[i]->clear();

        std::unique_lock<std::shared_mutex> lock(*bloom_locks_[i]);
        // 重置 BloomFilter
        bloom_filters_[i] = std::make_unique<BloomFilter>(1000000 / k_num_shards_);
    }
    // 重置总数
    size_.store(0, std::memory_order_relaxed);
}

std::vector<std::pair<std::string, EValue>> MemTable::get_all_entries() const
{
    // 获取所有分片的有序数据
    std::vector<std::vector<std::pair<std::string, EValue>>> shard_entries(k_num_shards_);
    size_t total_elements = 0;

    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        shard_entries[i] = tables_[i]->get_all_entries();
        total_elements += shard_entries[i].size();
    }

    // 使用 K 路归并排序（因为每个分片内部已有序）
    std::vector<std::pair<std::string, EValue>> result;
    result.reserve(total_elements);

    // 使用最小堆进行 K 路归并
    // 堆元素: (key, shard_index, element_index)
    using HeapEntry = std::tuple<std::string, size_t, size_t>;
    auto cmp = [](const HeapEntry &a, const HeapEntry &b)
    {
        return std::get<0>(a) > std::get<0>(b); // 最小堆
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> min_heap(cmp);

    // 初始化堆：每个分片的第一个元素入堆
    for (size_t i = 0; i < k_num_shards_; ++i)
    {
        if (!shard_entries[i].empty())
        {
            min_heap.emplace(shard_entries[i][0].first, i, 0);
        }
    }

    // K 路归并
    while (!min_heap.empty())
    {
        auto [key, shard_idx, elem_idx] = min_heap.top();
        min_heap.pop();

        result.push_back(std::move(shard_entries[shard_idx][elem_idx]));

        // 将该分片的下一个元素入堆
        if (elem_idx + 1 < shard_entries[shard_idx].size())
        {
            min_heap.emplace(shard_entries[shard_idx][elem_idx + 1].first, shard_idx, elem_idx + 1);
        }
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
    {
        std::shared_lock<std::shared_mutex> lock(*bloom_locks_[idx]);
        if (!bloom_filters_[idx]->may_contain(key))
        {
            throw std::out_of_range("Key not found");
        }
    }
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