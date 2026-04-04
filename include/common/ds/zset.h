#ifndef TINYKV_INCLUDE_COMMON_ZSET_H_
#define TINYKV_INCLUDE_COMMON_ZSET_H_

#include "skip_list.h"
#include <string>
#include <optional>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <functional>
#include "common/base/export.h"
/**
 * @brief 跳表复合键，用于解决同分数下的排序和成员唯一性问题。
 */
struct ZSetKey
{
    double score;
    std::string member;

    // 关键：定义比较逻辑。分数优先，分数相同时比较成员字符串。
    bool operator<(const ZSetKey &other) const
    {
        if (score != other.score)
            return score < other.score;
        return member < other.member;
    }

    bool operator>(const ZSetKey &other) const { return other < *this; }
    bool operator==(const ZSetKey &other) const
    {
        return score == other.score && member == other.member;
    }
};

/**
 * @brief 高性能、线程安全的有序集合 (ZSet) 实现。
 * 内部结合跳表（按分数排序）与哈希表（O(1) 索引成员分数）。
 */
class EYAKV_COMMON_API ZSet
{
public:
    /**
     * @brief 构造函数。
     * 初始化跳表比较逻辑：直接使用 ZSetKey 的比较运算符。
     */
    ZSet();

    ~ZSet() = default;

    /**
     * @brief 拷贝构造（线程安全）。
     * 拷贝时会对源对象加共享锁。
     */
    ZSet(const ZSet &other);

    /**
     * @brief 拷贝赋值（线程安全）。
     */
    ZSet &operator=(const ZSet &other);

    /**
     * @brief 移动语义（线程安全）。
     */
    ZSet(ZSet &&other) noexcept;
    ZSet &operator=(ZSet &&other) noexcept;

    /**
     * @brief 添加成员或更新其分数。
     * @param member 成员标识
     * @param score 分数值（double）
     */
    void zadd(const std::string &member, const std::string &score);

    /**
     * @brief 获取指定成员的分数。
     * @return 存在则返回分值，否则返回 nullopt。
     */
    std::optional<std::string> zscore(const std::string &member) const;

    /**
     * @brief 增加指定成员的分数。
     * @return 更新后的分值，若成员不存在则返回 nullopt。
     */
    std::optional<std::string> zincrby(const std::string &member, const std::string &increment);

    /**
     * @brief 删除指定成员。
     * @return 是否删除成功。
     */
    bool zrem(const std::string &member);

    /**
     * @brief 获取成员总数。
     */
    size_t zcard() const;

    /**
     * @brief 获取指定成员的排名（0-based，按分数从小到大）。
     */
    std::optional<size_t> zrank(const std::string &member) const;

    /**
     * @brief 按照分数范围获取成员。
     * @param min_score 最小分
     * @param max_score 最大分
     * @return 成员及其分数的列表。
     */
    std::vector<std::pair<std::string, std::string>> zrange_by_score(const std::string &min_score, const std::string &max_score) const;

    /**
     * @brief 按照排名范围获取成员。
     * @param start 起始排名
     * @param end 结束排名
     */
    std::vector<std::pair<std::string, std::string>> zrange_by_rank(size_t start, size_t end) const;

    /**
     * @brief 按照分数范围批量删除成员。
     * @return 被删除的数量。
     */
    size_t zrem_range_by_score(const std::string &min_score, const std::string &max_score);

    /**
     * @brief 按照排名范围删除成员。
     * @param start 起始排名（0-based，包含）
     * @param end 结束排名（0-based，包含）
     * @return 被删除的成员数量
     */
    size_t zrem_range_by_rank(size_t start, size_t end);

    /**
     * @brief 清空集合。
     */
    void zclear();

    /**
     * @brief 估算当前的内存占用。
     */
    size_t memory_usage() const;
    /**
     * @brief 遍历所有成员及其分数（按分数从小到大）。
     * @param callback 接收 (member, score) 的回调函数。
     */
    void for_each(std::function<void(const std::string &, double)> callback) const;

    /**
     * @brief 序列化接口。
     * 由于底层类型变化，序列化函数现在处理的是 ZSetKey 和双精度分数。
     */
    std::string serialize(
        std::function<std::string(const SkipList<ZSetKey, std::string> &)> serialize_skiplist_func,
        std::function<std::string(const std::unordered_map<std::string, double> &)> serialize_map_func) const;

    /**
     * @brief 反序列化接口。
     */
    void deserialize(
        const char *data, size_t &offset,
        std::function<void(const char *, size_t &, SkipList<ZSetKey, std::string> &)> deserialize_skiplist_func,
        std::function<void(const char *, size_t &, std::unordered_map<std::string, double> &)> deserialize_map_func);

    /**
     * @brief 获取成员数量。
     */
    size_t size() const
    {
        return skiplist_.size();
    }

private:
    // 跳表：存储有序关系。K 为复合键，V 为成员标识。
    SkipList<ZSetKey, std::string> skiplist_;
    // 哈希表：存储成员到分数的映射，提供 O(1) 查找能力。
    std::unordered_map<std::string, double> member_score_map_;

    // 读写锁：支持多读一写并发控制。
    mutable std::shared_mutex rw_mutex_;

    // 额外内存统计（如哈希表开销）
    std::atomic<size_t> size_extra_bytes_{0};

    // 内存统计辅助常数
    static constexpr size_t HASH_NODE_COST = 32;
};

#endif