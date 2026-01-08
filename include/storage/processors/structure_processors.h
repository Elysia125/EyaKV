#ifndef TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
#define TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_

#include "storage/processors/processor.h"
#include "common/serialization/serializer.h"
#include "common/types/value.h"
// String Processor
class StringProcessor : public ValueProcessor
{
private:
    bool set(Storage *storage, const std::string &key, const std::string &value, const uint64_t &ttl = 0);

public:
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;
    std::vector<uint8_t> get_supported_types() const override;
};

// Set Processor
class SetProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行 Set 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型 (kSAdd, kSRem, kSMembers)
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 Set 数据
     * @param storage 存储引擎实例
     * @param memtable 内存表
     * @param type 操作类型
     * @param key 键
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    /**
     * @brief 向集合添加元素
     * @param storage 存储引擎
     * @param key 键
     * @param member 成员
     * @return true 添加成功 (之前不存在), false 成员已存在
     */
    size_t s_add(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);

    /**
     * @brief 从集合移除元素
     * @param storage 存储引擎
     * @param key 键
     * @param member 成员
     * @return true 移除成功, false 成员不存在
     */
    size_t s_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);

    /**
     * @brief 获取集合所有成员
     * @param storage 存储引擎
     * @param key 键
     * @return 成员列表
     */
    std::vector<std::string> s_members(Storage *storage, const std::string &key);
};

// ZSet Processor
class ZSetProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行 ZSet 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 ZSet 数据
     * @param memtable 内存表
     * @param type 操作类型
     * @param key 键
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    // Helper methods for ZSet operations
    size_t z_add(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &score_members, const bool is_recover = false);
    size_t z_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);
    std::optional<std::string> z_score(Storage *storage, const std::string &key, const std::string &member);
    std::optional<size_t> z_rank(Storage *storage, const std::string &key, const std::string &member);
    size_t z_card(Storage *storage, const std::string &key);
    std::string z_incr_by(Storage *storage, const std::string &key, const std::string &increment, const std::string &member, const bool is_recover = false);
    std::vector<std::pair<std::string, EyaValue>> z_range_by_rank(Storage *storage, const std::string &key, long long start, long long end);
    std::vector<std::pair<std::string, EyaValue>> z_range_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max);
    size_t z_rem_by_rank(Storage *storage, const std::string &key, long long start, long long end, const bool is_recover = false);
    size_t z_rem_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max, const bool is_recover = false);
};

// Deque (List) Processor
class DequeProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行 List 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 List 数据
     * @param memtable 内存表
     * @param type 操作类型
     * @param key 键
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    size_t l_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover = false);
    std::optional<std::string> l_pop(Storage *storage, const std::string &key, const bool is_recover = false);
    size_t r_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover = false);
    std::optional<std::string> r_pop(Storage *storage, const std::string &key, const bool is_recover = false);
    std::vector<std::string> l_range(Storage *storage, const std::string &key, long long start, long long end);
    std::optional<std::string> l_get(Storage *storage, const std::string &key, long long index);
    size_t l_size(Storage *storage, const std::string &key);
    std::vector<std::string> l_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover = false);
    std::vector<std::string> r_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover = false);
};

// Hash Processor
class HashProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行 Hash 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 Hash 数据
     * @param memtable 内存表
     * @param type 操作类型
     * @param key 键
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    size_t h_set(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &field_values, const bool is_recover = false);
    std::optional<std::string> h_get(Storage *storage, const std::string &key, const std::string &field);
    size_t h_del(Storage *storage, const std::string &key, const std::vector<std::string> &fields, const bool is_recover = false);
    std::vector<std::string> h_keys(Storage *storage, const std::string &key);
    std::vector<std::string> h_values(Storage *storage, const std::string &key);
    std::unordered_map<std::string, std::string> h_entries(Storage *storage, const std::string &key);
};

#endif // TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
