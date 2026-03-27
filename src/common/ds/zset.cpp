#include "common/ds/zset.h"
#include <mutex>

// 自定义比较器：适配 SkipList 所需的比较函数指针
inline int compare_zset_keys(const ZSetKey &a, const ZSetKey &b)
{
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

ZSet::ZSet()
    : skiplist_(DEFAULT_MAX_LEVEL, DEFAULT_PROBABILITY, compare_zset_keys)
{
    size_extra_bytes_.store(sizeof(ZSet), std::memory_order_relaxed);
}

/**
 * @brief 拷贝构造函数
 * 需要对 other 加共享锁，防止拷贝过程中 other 被修改
 */
ZSet::ZSet(const ZSet &other)
    : skiplist_(other.skiplist_) // 调用 SkipList 的拷贝构造（需确保 SkipList 已实现）
{
    std::shared_lock<std::shared_mutex> lock(other.rw_mutex_);
    member_score_map_ = other.member_score_map_;
    size_extra_bytes_.store(other.size_extra_bytes_.load());
}

/**
 * @brief 拷贝赋值操作符
 */
ZSet &ZSet::operator=(const ZSet &other)
{
    if (this == &other)
        return *this;

    // 为了防止死锁，按内存地址顺序加锁
    if (this < &other)
    {
        std::unique_lock<std::shared_mutex> lock_this(rw_mutex_);
        std::shared_lock<std::shared_mutex> lock_other(other.rw_mutex_);
        skiplist_ = other.skiplist_;
        member_score_map_ = other.member_score_map_;
        size_extra_bytes_.store(other.size_extra_bytes_.load());
    }
    else
    {
        std::shared_lock<std::shared_mutex> lock_other(other.rw_mutex_);
        std::unique_lock<std::shared_mutex> lock_this(rw_mutex_);
        skiplist_ = other.skiplist_;
        member_score_map_ = other.member_score_map_;
        size_extra_bytes_.store(other.size_extra_bytes_.load());
    }
    return *this;
}

/**
 * @brief 移动构造函数
 * 窃取 other 的资源，并将 other 重置
 */
ZSet::ZSet(ZSet &&other) noexcept
    : skiplist_(std::move(other.skiplist_))
{
    std::unique_lock<std::shared_mutex> lock(other.rw_mutex_);
    member_score_map_ = std::move(other.member_score_map_);
    size_extra_bytes_.store(other.size_extra_bytes_.load());

    // 重置 source
    other.size_extra_bytes_.store(sizeof(ZSet));
}

/**
 * @brief 移动赋值操作符
 */
ZSet &ZSet::operator=(ZSet &&other) noexcept
{
    if (this == &other)
        return *this;

    // 同样需要双重锁定
    if (this < &other)
    {
        std::unique_lock<std::shared_mutex> lock_this(rw_mutex_);
        std::unique_lock<std::shared_mutex> lock_other(other.rw_mutex_);
        skiplist_ = std::move(other.skiplist_);
        member_score_map_ = std::move(other.member_score_map_);
        size_extra_bytes_.store(other.size_extra_bytes_.load());
        other.size_extra_bytes_.store(sizeof(ZSet));
    }
    else
    {
        std::unique_lock<std::shared_mutex> lock_other(other.rw_mutex_);
        std::unique_lock<std::shared_mutex> lock_this(rw_mutex_);
        skiplist_ = std::move(other.skiplist_);
        member_score_map_ = std::move(other.member_score_map_);
        size_extra_bytes_.store(other.size_extra_bytes_.load());
        other.size_extra_bytes_.store(sizeof(ZSet));
    }
    return *this;
}

void ZSet::zadd(const std::string &member, const std::string &sc)
{
    double score = std::stod(sc);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    auto it = member_score_map_.find(member);
    if (it != member_score_map_.end())
    {
        if (it->second == score)
            return; // 分数没变，无需更新

        // 1. 分数变了，先从跳表中移除旧分数对应的节点
        skiplist_.remove(ZSetKey{it->second, member});
        // 修正内存统计：减去旧分数字符串假设的开销（这里直接按比例处理）
        size_extra_bytes_.fetch_sub(sizeof(double), std::memory_order_relaxed);
    }
    else
    {
        // 新成员，计入哈希表基础开销
        size_extra_bytes_.fetch_add(member.capacity() + HASH_NODE_COST, std::memory_order_relaxed);
    }

    // 2. 插入新分数到跳表
    skiplist_.insert(ZSetKey{score, member}, member);

    // 3. 更新哈希表
    member_score_map_[member] = score;
    size_extra_bytes_.fetch_add(sizeof(double), std::memory_order_relaxed);
}

std::optional<std::string> ZSet::zscore(const std::string &member) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = member_score_map_.find(member);
    if (it != member_score_map_.end())
    {
        return std::to_string(it->second);
    }
    return std::nullopt;
}

bool ZSet::zrem(const std::string &member)
{
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
        return false;

    double score = it->second;
    // 从跳表物理删除
    skiplist_.remove(ZSetKey{score, member});

    // 扣除内存统计
    size_extra_bytes_.fetch_sub(member.capacity() + HASH_NODE_COST + sizeof(double), std::memory_order_relaxed);

    // 从哈希表删除
    member_score_map_.erase(it);
    return true;
}

std::optional<size_t> ZSet::zrank(const std::string &member) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
        return std::nullopt;

    // 直接调用跳表的排名查询
    return skiplist_.rank(ZSetKey{it->second, member});
}

std::vector<std::pair<std::string, std::string>> ZSet::zrange_by_score(const std::string &min_sc, const std::string &max_sc) const
{
    double min_score = std::stod(min_sc);
    double max_score = std::stod(max_sc);
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    // 使用范围边界 Key
    ZSetKey start_key{min_score, ""};
    ZSetKey end_key{max_score, "\xff"}; // 使用高位字符确保覆盖该分数下的所有成员

    auto raw_res = skiplist_.range_by_key(start_key, end_key);

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(raw_res.size());
    for (const auto &item : raw_res)
    {
        // item.first 是 ZSetKey, item.second 是 member string
        result.emplace_back(item.second, std::to_string(item.first.score));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> ZSet::zrange_by_rank(size_t start, size_t end) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    auto raw_res = skiplist_.range_by_rank(start, end);

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(raw_res.size());
    for (const auto &item : raw_res)
    {
        // item.first 是 ZSetKey, item.second 是 member string
        result.emplace_back(item.second, std::to_string(item.first.score));
    }
    return result;
}

size_t ZSet::zrem_range_by_score(const std::string &min_sc, const std::string &max_sc)
{
    double min_score = std::stod(min_sc);
    double max_score = std::stod(max_sc);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    ZSetKey start_key{min_score, ""};
    ZSetKey end_key{max_score, "\xff"};

    // 先找出范围内成员，用于同步删除哈希表
    auto to_remove = skiplist_.range_by_key(start_key, end_key);
    for (const auto &item : to_remove)
    {
        member_score_map_.erase(item.second);
        size_extra_bytes_.fetch_sub(item.second.capacity() + HASH_NODE_COST + sizeof(double), std::memory_order_relaxed);
    }

    return skiplist_.remove_range_by_key(start_key, end_key);
}

size_t ZSet::zrem_range_by_rank(size_t start, size_t end)
{
    if (start > end)
        return 0;

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    // 1. 获取目标排名范围内的所有元素
    // 注意：这里调用的是 SkipList 的 range_by_rank
    auto to_remove = skiplist_.range_by_rank(start, end);
    if (to_remove.empty())
        return 0;

    size_t removed_count = 0;
    for (const auto &item : to_remove)
    {
        // item.first 是 ZSetKey (包含 score 和 member)
        // item.second 是 member string
        const ZSetKey &key = item.first;
        const std::string &member = item.second;

        // 2. 从哈希表中删除并更新内存计数
        auto it = member_score_map_.find(member);
        if (it != member_score_map_.end())
        {
            size_extra_bytes_.fetch_sub(member.capacity() + HASH_NODE_COST + sizeof(double),
                                        std::memory_order_relaxed);
            member_score_map_.erase(it);
        }
    }

    return skiplist_.remove_range_by_rank(start, end);
}

std::optional<std::string> ZSet::zincrby(const std::string &member, const std::string &incr)
{
    double increment = std::stod(incr);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
        return std::nullopt;

    double old_score = it->second;
    double new_score = old_score + increment;

    // 移动跳表位置：必须先删后插，因为 Key 结构发生了变化
    skiplist_.remove(ZSetKey{old_score, member});
    skiplist_.insert(ZSetKey{new_score, member}, member);

    it->second = new_score;
    return std::to_string(new_score);
}

void ZSet::zclear()
{
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    skiplist_.clear();
    member_score_map_.clear();
    size_extra_bytes_.store(sizeof(ZSet), std::memory_order_relaxed);
}

size_t ZSet::zcard() const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return member_score_map_.size();
}

size_t ZSet::memory_usage() const
{
    // 总内存 = 对象基础大小 + 哈希表动态开销 + 跳表动态开销
    return size_extra_bytes_.load(std::memory_order_relaxed) + skiplist_.memory_usage();
}

void ZSet::for_each(std::function<void(const std::string &, double)> callback) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    // 假设 SkipList 提供了 for_each 接口，或者我们通过底层迭代
    // 这里演示通过跳表的 for_each 适配
    skiplist_.for_each([&](const ZSetKey &key, const std::string &member)
                       { callback(member, key.score); });
}

std::string ZSet::serialize(
    std::function<std::string(const SkipList<ZSetKey, std::string> &)> serialize_skiplist_func,
    std::function<std::string(const std::unordered_map<std::string, double> &)> serialize_map_func) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::string result;

    // 1. 调用传入的跳表序列化逻辑
    result.append(serialize_skiplist_func(skiplist_));
    // 2. 调用传入的哈希表序列化逻辑
    result.append(serialize_map_func(member_score_map_));

    return result;
}

void ZSet::deserialize(
    const char *data, size_t &offset,
    std::function<void(const char *, size_t &, SkipList<ZSetKey, std::string> &)> deserialize_skiplist_func,
    std::function<void(const char *, size_t &, std::unordered_map<std::string, double> &)> deserialize_map_func)
{
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    // 1. 清空旧数据
    skiplist_.clear();
    member_score_map_.clear();

    // 2. 执行反序列化回调
    deserialize_skiplist_func(data, offset, skiplist_);
    deserialize_map_func(data, offset, member_score_map_);

    // 3. 重新计算内存占用（遍历一遍 map 进行估算）
    size_t new_extra_size = sizeof(ZSet);
    for (const auto &pair : member_score_map_)
    {
        new_extra_size += (pair.first.capacity() + HASH_NODE_COST + sizeof(double));
    }
    size_extra_bytes_.store(new_extra_size, std::memory_order_relaxed);
}