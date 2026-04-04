#ifndef TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
#define TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_

#include "storage/processors/processor.h"
#include "common/serialization/serializer.h"
#include "common/types/value.h"

// String Processor
class StringProcessor : public ValueProcessor
{
private:
    /**
     * @brief 设置字符串类型的键值对
     * @param storage 存储引擎实例
     * @param key 键名
     * @param value 键值
     * @param ttl 过期时间（秒），0 表示永不过期
     * @return true 表示设置成功
     */
    bool set(Storage *storage, const std::string &key, const std::string &value, const uint64_t &ttl = 0);

    /**
     * @brief 设置字符串类型的键值对 (std::string_view 重载版本)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param value 键值
     * @param ttl 过期时间（秒），0 表示永不过期
     * @return true 表示设置成功
     */
    bool set(Storage *storage, const std::string_view key, const std::string_view value, const uint64_t &ttl = 0);

public:
    /**
     * @brief 执行 String 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型 (如 kSet)
     * @param args 命令参数列表
     * @return Response 执行结果，包含成功标志及可能的返回值/错误信息
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 执行 String 相关命令 (std::string_view 重载版本)
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args) override;

    /**
     * @brief 恢复 String 数据 (用于 WAL/持久化恢复)
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param key 键名
     * @param payload 序列化后的数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

    /**
     * @brief 获取当前处理器支持的操作类型列表
     * @return 包含支持的 OperationType 的数组
     */
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
     * @param args 命令参数列表
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 执行 Set 相关命令 (std::string_view 重载版本)
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 Set 数据 (用于 WAL/持久化恢复)
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param key 键名
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    /**
     * @brief 只读获取 Set 类型的元数据 (Metadata)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的元数据对象
     * @param meta_val 输出参数：原始包装的 EValue
     * @return true 成功读取有效元数据，false 键不存在或已过期/删除
     */
    bool set_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val);

    /**
     * @brief 获取或初始化创建 Set 类型的元数据
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的或新建的元数据对象
     * @param meta_val 输出参数：原有的 EValue（若为新创建则无有效数据）
     * @param is_new 输出参数：指示是否为全新创建的集合
     */
    void set_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new);

    /**
     * @brief 向无序集合添加一个或多个成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param members 待添加的成员列表
     * @param is_recover 是否属于数据恢复过程 (避免重复写 WAL)
     * @return 实际成功添加到集合中的新成员数量
     */
    size_t s_add(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);

    /**
     * @brief 向无序集合添加一个或多个成员 (std::string_view 重载版本)
     */
    size_t s_add(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover = false);

    /**
     * @brief 从无序集合移除一个或多个成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param members 待移除的成员列表
     * @param is_recover 是否属于数据恢复过程
     * @return 实际成功移除的成员数量
     */
    size_t s_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);

    /**
     * @brief 从无序集合移除一个或多个成员 (std::string_view 重载版本)
     */
    size_t s_rem(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover = false);

    /**
     * @brief 获取无序集合的所有成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 包含所有成员的数组
     */
    std::vector<std::string> s_members(Storage *storage, const std::string &key);

    /**
     * @brief 获取无序集合的所有成员 (std::string_view 重载版本)
     */
    std::vector<std::string> s_members(Storage *storage, const std::string_view key);
};

// ZSet Processor
class ZSetProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行有序集合 (ZSet) 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 执行有序集合 (ZSet) 相关命令 (std::string_view 重载版本)
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 ZSet 数据 (用于 WAL/持久化恢复)
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param key 键名
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    /**
     * @brief 获取或初始化创建 ZSet 类型的元数据
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的或新建的元数据对象
     * @param meta_val 输出参数：原有的 EValue（若为新创建则无有效数据）
     * @param is_new 输出参数：指示是否为全新创建的有序集合
     */
    void zset_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new);

    /**
     * @brief 只读获取 ZSet 类型的元数据 (Metadata)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的元数据对象
     * @param meta_val 输出参数：原始包装的 EValue
     * @return true 成功读取有效元数据，false 键不存在或已过期/删除
     */
    bool zset_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val);

    /**
     * @brief 向有序集合添加一个或多个带有分数的成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param score_members 包含 <分数, 成员> 对的数组
     * @param is_recover 是否属于数据恢复过程
     * @return 成功添加到集合中的新成员数量（不包含仅更新分数的成员）
     */
    size_t z_add(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &score_members, const bool is_recover = false);
    size_t z_add(Storage *storage, const std::string_view key, const std::vector<std::pair<std::string_view, std::string_view>> &score_members, const bool is_recover = false);

    /**
     * @brief 从有序集合移除一个或多个成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param members 待移除的成员列表
     * @param is_recover 是否属于数据恢复过程
     * @return 实际成功移除的成员数量
     */
    size_t z_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover = false);
    size_t z_rem(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover = false);

    /**
     * @brief 获取有序集合中指定成员的分数
     * @param storage 存储引擎实例
     * @param key 键名
     * @param member 成员名
     * @return 成功则返回分数字符串，否则返回 std::nullopt
     */
    std::optional<std::string> z_score(Storage *storage, const std::string &key, const std::string &member);
    std::optional<std::string> z_score(Storage *storage, const std::string_view key, const std::string_view member);

    /**
     * @brief 获取有序集合中指定成员的排名 (从小到大)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param member 成员名
     * @return 成功则返回排名索引(基于0)，否则返回 std::nullopt
     */
    std::optional<size_t> z_rank(Storage *storage, const std::string &key, const std::string &member);
    std::optional<size_t> z_rank(Storage *storage, const std::string_view key, const std::string_view member);

    /**
     * @brief 获取有序集合的元素数量
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 元素总数
     */
    size_t z_card(Storage *storage, const std::string &key);
    size_t z_card(Storage *storage, const std::string_view key);

    /**
     * @brief 为有序集合中的指定成员增加分数
     * @param storage 存储引擎实例
     * @param key 键名
     * @param increment 增量分数值(字符串形式)
     * @param member 成员名
     * @param is_recover 是否属于数据恢复过程
     * @return 更新后的分数字符串
     */
    std::string z_incr_by(Storage *storage, const std::string &key, const std::string &increment, const std::string &member, const bool is_recover = false);
    std::string z_incr_by(Storage *storage, const std::string_view key, const std::string_view increment, const std::string_view member, const bool is_recover = false);

    /**
     * @brief 按照排名范围获取有序集合中的成员和分数
     * @param storage 存储引擎实例
     * @param key 键名
     * @param start 起始排名 (支持负数)
     * @param end 结束排名 (支持负数)
     * @return 符合指定范围的 <成员, 分数> 键值对数组
     */
    std::vector<std::pair<std::string, EyaValue>> z_range_by_rank(Storage *storage, const std::string &key, long long start, long long end);
    std::vector<std::pair<std::string, EyaValue>> z_range_by_rank(Storage *storage, const std::string_view key, long long start, long long end);

    /**
     * @brief 按照分数范围获取有序集合中的成员和分数
     * @param storage 存储引擎实例
     * @param key 键名
     * @param min 最小分数 (包含)
     * @param max 最大分数 (包含)
     * @return 符合指定分数范围的 <成员, 分数> 键值对数组
     */
    std::vector<std::pair<std::string, EyaValue>> z_range_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max);
    std::vector<std::pair<std::string, EyaValue>> z_range_by_score(Storage *storage, const std::string_view key, const std::string_view min, const std::string_view max);

    /**
     * @brief 按照排名范围移除有序集合中的成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param start 起始排名 (支持负数)
     * @param end 结束排名 (支持负数)
     * @param is_recover 是否属于数据恢复过程
     * @return 实际被移除的元素数量
     */
    size_t z_rem_by_rank(Storage *storage, const std::string &key, long long start, long long end, const bool is_recover = false);
    size_t z_rem_by_rank(Storage *storage, const std::string_view key, long long start, long long end, const bool is_recover = false);

    /**
     * @brief 按照分数范围移除有序集合中的成员
     * @param storage 存储引擎实例
     * @param key 键名
     * @param min 最小分数 (包含)
     * @param max 最大分数 (包含)
     * @param is_recover 是否属于数据恢复过程
     * @return 实际被移除的元素数量
     */
    size_t z_rem_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max, const bool is_recover = false);
    size_t z_rem_by_score(Storage *storage, const std::string_view key, const std::string_view min, const std::string_view max, const bool is_recover = false);
};

// Deque (List) Processor
class DequeProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行双端队列/列表 (List) 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 执行双端队列/列表 (List) 相关命令 (std::string_view 重载版本)
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 List 数据 (用于 WAL/持久化恢复)
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param key 键名
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    /**
     * @brief 只读获取 List 类型的元数据 (Metadata)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的元数据对象
     * @param meta_val 输出参数：原始包装的 EValue
     * @return true 成功读取有效元数据，false 键不存在或已过期/删除
     */
    bool deque_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val);

    /**
     * @brief 获取或初始化创建 List 类型的元数据
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的或新建的元数据对象
     * @param meta_val 输出参数：原有的 EValue
     * @param is_new 输出参数：指示是否为全新创建的列表
     */
    void deque_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new);

    /**
     * @brief 从列表左侧（头部）推入一个或多个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param values 待推入的元素数组
     * @param is_recover 是否属于数据恢复过程
     * @return 执行操作后列表的长度
     */
    size_t l_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover = false);
    size_t l_push(Storage *storage, const std::string_view key, const std::vector<std::string_view> &values, const bool is_recover = false);

    /**
     * @brief 从列表左侧（头部）弹出一个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param is_recover 是否属于数据恢复过程
     * @return 弹出的元素值，如果列表为空则返回 std::nullopt
     */
    std::optional<std::string> l_pop(Storage *storage, const std::string &key, const bool is_recover = false);
    std::optional<std::string> l_pop(Storage *storage, const std::string_view key, const bool is_recover = false);

    /**
     * @brief 从列表右侧（尾部）推入一个或多个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param values 待推入的元素数组
     * @param is_recover 是否属于数据恢复过程
     * @return 执行操作后列表的长度
     */
    size_t r_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover = false);
    size_t r_push(Storage *storage, const std::string_view key, const std::vector<std::string_view> &values, const bool is_recover = false);

    /**
     * @brief 从列表右侧（尾部）弹出一个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param is_recover 是否属于数据恢复过程
     * @return 弹出的元素值，如果列表为空则返回 std::nullopt
     */
    std::optional<std::string> r_pop(Storage *storage, const std::string &key, const bool is_recover = false);
    std::optional<std::string> r_pop(Storage *storage, const std::string_view key, const bool is_recover = false);

    /**
     * @brief 获取列表指定索引范围内的元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param start 起始索引 (支持负数)
     * @param end 结束索引 (支持负数)
     * @return 范围内的元素数组
     */
    std::vector<std::string> l_range(Storage *storage, const std::string &key, long long start, long long end);
    std::vector<std::string> l_range(Storage *storage, const std::string_view key, long long start, long long end);

    /**
     * @brief 通过索引获取列表中的单个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param index 目标索引 (支持负数)
     * @return 对应的元素值，若索引越界则返回 std::nullopt
     */
    std::optional<std::string> l_get(Storage *storage, const std::string &key, long long index);
    std::optional<std::string> l_get(Storage *storage, const std::string_view key, long long index);

    /**
     * @brief 获取列表当前长度
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 列表元素总数
     */
    size_t l_size(Storage *storage, const std::string &key);
    size_t l_size(Storage *storage, const std::string_view key);

    /**
     * @brief 从列表左侧（头部）批量弹出多个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param n 弹出的元素数量
     * @param is_recover 是否属于数据恢复过程
     * @return 弹出的元素数组（数量可能小于 n，取决于实际列表长度）
     */
    std::vector<std::string> l_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover = false);
    std::vector<std::string> l_pop_n(Storage *storage, const std::string_view key, size_t n, const bool is_recover = false);

    /**
     * @brief 从列表右侧（尾部）批量弹出多个元素
     * @param storage 存储引擎实例
     * @param key 键名
     * @param n 弹出的元素数量
     * @param is_recover 是否属于数据恢复过程
     * @return 弹出的元素数组（数量可能小于 n，取决于实际列表长度）
     */
    std::vector<std::string> r_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover = false);
    std::vector<std::string> r_pop_n(Storage *storage, const std::string_view key, size_t n, const bool is_recover = false);
};

// Hash Processor
class HashProcessor : public ValueProcessor
{
public:
    /**
     * @brief 执行哈希表 (Hash) 相关命令
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param args 命令参数
     * @return Response 执行结果
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args) override;

    /**
     * @brief 执行哈希表 (Hash) 相关命令 (std::string_view 重载版本)
     */
    Response execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args) override;

    /**
     * @brief 获取支持的操作类型
     * @return 支持的 OperationType 列表
     */
    std::vector<uint8_t> get_supported_types() const override;

    /**
     * @brief 恢复 Hash 数据 (用于 WAL/持久化恢复)
     * @param storage 存储引擎实例
     * @param type 操作类型
     * @param key 键名
     * @param payload 数据载荷
     * @return true 成功, false 失败
     */
    bool recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload) override;

private:
    /**
     * @brief 只读获取 Hash 类型的元数据 (Metadata)
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的元数据对象
     * @param meta_val 输出参数：原始包装的 EValue
     * @return true 成功读取有效元数据，false 键不存在或已过期/删除
     */
    bool hash_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val);

    /**
     * @brief 获取或初始化创建 Hash 类型的元数据
     * @param storage 存储引擎实例
     * @param key 键名
     * @param meta 输出参数：解析出的或新建的元数据对象
     * @param meta_val 输出参数：原有的 EValue
     * @param is_new 输出参数：指示是否为全新创建的哈希表
     */
    void hash_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new);

    /**
     * @brief 在哈希表中设置一个或多个字段的值
     * @param storage 存储引擎实例
     * @param key 键名
     * @param field_values 包含 <字段名, 字段值> 的键值对数组
     * @param is_recover 是否属于数据恢复过程
     * @return 实际新创建的字段数量 (如果是更新旧字段不计入)
     */
    size_t h_set(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &field_values, const bool is_recover = false);
    size_t h_set(Storage *storage, const std::string_view key, const std::vector<std::pair<std::string_view, std::string_view>> &field_values, const bool is_recover = false);

    /**
     * @brief 获取哈希表中指定字段的值
     * @param storage 存储引擎实例
     * @param key 键名
     * @param field 字段名
     * @return 如果存在返回字段对应的值，否则返回 std::nullopt
     */
    std::optional<std::string> h_get(Storage *storage, const std::string &key, const std::string &field);
    std::optional<std::string> h_get(Storage *storage, const std::string_view key, const std::string_view field);

    /**
     * @brief 从哈希表中删除一个或多个字段
     * @param storage 存储引擎实例
     * @param key 键名
     * @param fields 待删除的字段名数组
     * @param is_recover 是否属于数据恢复过程
     * @return 实际成功删除的字段数量
     */
    size_t h_del(Storage *storage, const std::string &key, const std::vector<std::string> &fields, const bool is_recover = false);
    size_t h_del(Storage *storage, const std::string_view key, const std::vector<std::string_view> &fields, const bool is_recover = false);

    /**
     * @brief 获取哈希表中所有的字段名 (Keys)
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 包含所有字段名的数组
     */
    std::vector<std::string> h_keys(Storage *storage, const std::string &key);
    std::vector<std::string> h_keys(Storage *storage, const std::string_view key);

    /**
     * @brief 获取哈希表中所有的字段值 (Values)
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 包含所有值的数组
     */
    std::vector<std::string> h_values(Storage *storage, const std::string &key);
    std::vector<std::string> h_values(Storage *storage, const std::string_view key);

    /**
     * @brief 获取哈希表中所有的字段和值
     * @param storage 存储引擎实例
     * @param key 键名
     * @return 包含所有 <字段, 值> 映射关系的 unordered_map
     */
    std::unordered_map<std::string, std::string> h_entries(Storage *storage, const std::string &key);
    std::unordered_map<std::string, std::string> h_entries(Storage *storage, const std::string_view key);
};

#endif // TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
