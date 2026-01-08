#ifndef TINYKV_INCLUDE_COMMON_ZSET_H_
#define TINYKV_INCLUDE_COMMON_ZSET_H_

#include "skip_list.h"
#include <string>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "common/base/export.h"
#include "common/util/utils.h"

inline int compare_double_strings(const std::string &a, const std::string &b)
{
    std::string a_clean = a.substr(0, a.find(" "));
    std::string b_clean = b.substr(0, b.find(" "));
    double da = std::stod(a_clean);
    double db = std::stod(b_clean);
    return (da < db) ? -1 : ((da > db) ? 1 : 0);
}
/**
 * @brief ZSet 类实现了一个基于跳表的有序集合数据结构。
 *
 * 每个元素由一个成员（字符串）和一个分值（双精度浮点数）组成。
 * 内部使用跳表按分值排序，同时使用哈希表实现成员到分值的快速映射。
 */
class EYAKV_COMMON_API ZSet
{
public:
    ZSet() : skiplist_(DEFAULT_MAX_LEVEL, DEFAULT_PROBABILITY, compare_double_strings, calculateStringSize, calculateStringSize), size_exclude_skiplist(0)
    {
        size_exclude_skiplist.fetch_add(sizeof(ZSet), std::memory_order_relaxed);
    }
    ~ZSet() = default;

    ZSet(const ZSet &other) noexcept : skiplist_(other.skiplist_), member_score_map_(other.member_score_map_)
    {
    }
    ZSet &operator=(const ZSet &other) noexcept
    {
        skiplist_ = other.skiplist_;
        member_score_map_ = other.member_score_map_;
        return *this;
    }
    // 允许移动
    ZSet(ZSet &&other) noexcept = default;
    ZSet &operator=(ZSet &&other) noexcept = default;

    /**
     * @brief 添加一个元素，或更新已存在元素的分值。
     * @param member 元素成员
     * @param score 分值
     */
    void zadd(const std::string &member, const std::string &score);

    /**
     * @brief 获取有序集合中指定成员的分值。
     * @param member 元素成员
     * @return 如果成员存在，返回对应的分值；否则返回 std::nullopt。
     */
    std::optional<std::string> zscore(const std::string &member) const;
    /**
     * @brief 删除有序集合中的指定成员。
     * @param member 元素成员
     * @return 如果成员存在并被删除，返回 true；否则返回 false。
     */
    bool zrem(const std::string &member);
    /**
     * @brief 获取元素的数量。
     * @return 元素数量
     */
    size_t zcard() const;
    /**
     * @brief 获取所占空间的大小
     */
    size_t memory_usage() const
    {
        return size_exclude_skiplist.load(std::memory_order_relaxed) + skiplist_.memory_usage();
    }
    /**
     * @brief 清空集合。
     */
    void zclear();
    /**
     * @brief 按照分值范围获取成员列表。
     * @param min_score 最小分值（包含）
     * @param max_score 最大分值（包含）
     * @return 符合分值范围的成员列表
     */
    std::vector<std::pair<std::string, std::string>> zrange_by_score(const std::string &min_score, const std::string &max_score) const;

    /**
     * @brief 按照排名范围获取成员列表。
     * @param start_rank 起始排名（0-based，包含）
     * @param end_rank 结束排名（0-based，包含）
     */
    std::vector<std::pair<std::string, std::string>> zrange_by_rank(size_t start_rank, size_t end_rank) const;

    /**
     * @brief 获取指定成员的排名（按分值从小到大排序，0-based）。
     * @param member 元素成员
     * @return 如果成员存在，返回对应的排名；否则返回 std::nullopt。
     */
    std::optional<size_t> zrank(const std::string &member) const;

    /**
     * @brief 按照分值范围删除成员。
     * @param min_score 最小分值（包含）
     * @param max_score 最大分值（包含）
     * @return 被删除的成员数量
     */
    size_t zrem_range_by_score(const std::string &min_score, const std::string &max_score);

    /**
     * @brief 按照排名范围删除成员。
     * @param start_rank 起始排名（0-based，包含）
     * @param end_rank 结束排名（0-based，包含）
     * @return 被删除的成员数量
     */
    size_t zrem_range_by_rank(size_t start_rank, size_t end_rank);

    /**
     * @brief 序列化
     */
    std::string serialize(std::string (*serialize_skiplist_func)(const SkipList<std::string, std::string> &),
                          std::string (*serialize_map_func)(const std::unordered_map<std::string, std::string> &)) const;

    /**
     * @brief 反序列化
     */
    void deserialize(const char *data, size_t &offset,
                     void (*deserialize_skiplist_func)(const char *, size_t &, SkipList<std::string, std::string> &),
                     void (*deserialize_map_func)(const char *, size_t &, std::unordered_map<std::string, std::string> &));
    /**
     * @brief 遍历
     */
    void for_each(std::function<void(const std::string &, const std::string &)> callback) const;
    /**
     * @brief 增加分数
     */
    std::optional<std::string> z_incrby(const std::string &member, const std::string &increment);

    /**
     * @brief 清空
     */
    void z_clear()
    {
        skiplist_.clear();
        member_score_map_.clear();
    }

private:
    SkipList<std::string, std::string> skiplist_;                   // 按分值排序的跳表
    std::unordered_map<std::string, std::string> member_score_map_; // 成员到分值的映射
    std::mutex mutex_;
    std::atomic<size_t> size_exclude_skiplist{0};
};
#endif