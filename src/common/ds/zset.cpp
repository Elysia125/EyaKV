#include "common/ds/zset.h"
#include <mutex>
#include <vector>
#include <utility>
#include <unordered_map>
#include "common/types/value.h"
#include <optional>
void ZSet::zadd(const std::string &member, const std::string &score)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = member_score_map_.find(member);
    if (it != member_score_map_.end())
    {
        skiplist_.remove(it->second + " " + member);
    }
    // 1. 更新跳表
    skiplist_.insert(score + " " + member, member); // 使用分值+成员作为复合键，确保唯一性
    // 2. 更新成员-分值映射
    member_score_map_[member] = score;
    size_exclude_skiplist.fetch_add(calculateStringSize(member) + calculateStringSize(score) + HASH_COST, std::memory_order_relaxed);
}

std::optional<std::string> ZSet::zscore(const std::string &member) const
{
    auto it = member_score_map_.find(member);
    if (it != member_score_map_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

bool ZSet::zrem(const std::string &member)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
    {
        return false; // 成员不存在
    }
    std::string score = it->second;
    // 1. 从跳表删除
    bool removed = skiplist_.remove(score + " " + member);
    if (removed)
    {
        // 2. 从成员-分值映射删除
        member_score_map_.erase(it);
        size_exclude_skiplist.fetch_sub(calculateStringSize(member) + calculateStringSize(score) + HASH_COST, std::memory_order_relaxed);
    }
    return removed;
}

size_t ZSet::zcard() const
{
    return member_score_map_.size();
}

void ZSet::zclear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    skiplist_.clear();
    member_score_map_.clear();
}

std::vector<std::pair<std::string, std::string>> ZSet::zrange_by_score(const std::string &min_score, const std::string &max_score) const
{
    std::string min = min_score + " ";
    std::string max = max_score + " ";
    std::vector<std::pair<std::string, std::string>> result = skiplist_.range_by_key(min, max);
    for (auto &item : result)
    {
        item.first = item.first.substr(0, item.first.find(" "));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> ZSet::zrange_by_rank(size_t start_rank, size_t end_rank) const
{
    std::vector<std::pair<std::string, std::string>> result = skiplist_.range_by_rank(start_rank, end_rank);
    for (auto &item : result)
    {
        item.first = item.first.substr(0, item.first.find(" "));
    }
    return result;
}

std::optional<size_t> ZSet::zrank(const std::string &member) const
{
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
    {
        return std::nullopt;
    }
    std::string score = it->second;
    std::string key = score + " " + member;
    return skiplist_.rank(key);
}

size_t ZSet::zrem_range_by_score(const std::string &min_score, const std::string &max_score)
{
    std::string min = min_score + " ";
    std::string max = max_score + " ";
    std::vector<std::pair<std::string, std::string>> result = skiplist_.range_by_key(min, max);
    size_t removed_size = 0;
    for (auto &item : result)
    {
        member_score_map_.erase(item.second);
        removed_size += calculateStringSize(item.second) + calculateStringSize(item.first) + HASH_COST;
    }
    size_exclude_skiplist.fetch_sub(removed_size, std::memory_order_relaxed);
    return skiplist_.remove_range_by_key(min, max);
}

size_t ZSet::zrem_range_by_rank(size_t start_rank, size_t end_rank)
{
    std::vector<std::pair<std::string, std::string>> result = skiplist_.range_by_rank(start_rank, end_rank);
    size_t removed_size = 0;
    for (auto &item : result)
    {
        member_score_map_.erase(item.second);
        removed_size += calculateStringSize(item.second) + calculateStringSize(item.first) + HASH_COST;
    }
    size_exclude_skiplist.fetch_sub(removed_size, std::memory_order_relaxed);
    return skiplist_.remove_range_by_rank(start_rank, end_rank);
}

std::string ZSet::serialize(std::string (*serialize_skiplist_func)(const SkipList<std::string, std::string> &),
                            std::string (*serialize_map_func)(const std::unordered_map<std::string, std::string> &)) const
{
    std::string result;
    result.append(serialize_skiplist_func(skiplist_));
    result.append(serialize_map_func(member_score_map_));
    return result;
}
/**
 * @brief 反序列化
 */
void ZSet::deserialize(const char *data, size_t &offset,
                       void (*deserialize_skiplist_func)(const char *, size_t &, SkipList<std::string, std::string> &),
                       void (*deserialize_map_func)(const char *, size_t &, std::unordered_map<std::string, std::string> &))
{
    deserialize_skiplist_func(data, offset, skiplist_);
    deserialize_map_func(data, offset, member_score_map_);
}

void ZSet::for_each(std::function<void(const std::string &, const std::string &)> callback) const
{
    skiplist_.for_each(callback);
}

std::optional<std::string> ZSet::z_incrby(const std::string &member, const std::string &increment)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = member_score_map_.find(member);
    if (it == member_score_map_.end())
    {
        return std::nullopt;
    }
    std::string score = it->second;
    std::string key = score + " " + member;
    skiplist_.remove(key);
    std::string new_score = std::to_string(std::stod(score) + std::stod(increment));
    skiplist_.insert(new_score + " " + member, member);
    member_score_map_[member] = new_score;
    return new_score;
}