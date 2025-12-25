#ifndef COMMON_H
#define COMMON_H

#include "zset.h"
#include "serializer.h"
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring>

#define HASH_COST 32

using EyaValue = std::variant<std::string,
                              std::vector<std::string>,
                              std::unordered_set<std::string>,
                              std::unordered_map<std::string, std::string>,
                              ZSet>;
typedef EyaKV::Serializer Serializer;
inline std::string serialize_eya_value(const EyaValue &value)
{
    std::string result;
    // 先写入类型索引（1字节）
    uint8_t type_index = static_cast<uint8_t>(value.index());
    result.append(reinterpret_cast<const char *>(&type_index), sizeof(type_index));
    // 再写入序列化后的值
    result.append(std::visit([](auto &&arg) -> std::string
                             { return Serializer::serialize(arg); }, value));
    return result;
}

inline EyaValue deserialize_eya_value(const char *data, size_t &offset)
{
    uint8_t type_index;
    std::memcpy(&type_index, data + offset, sizeof(type_index));
    offset += sizeof(type_index);
    size_t index = type_index;

    switch (index)
    {
    case 0:
    {
        return Serializer::deserializeString(data, offset);
    }
    case 1:
    {
        std::vector<std::string> vec;
        Serializer::deserializeVector(data, offset, vec);
        return vec;
    }
    case 2:
    {
        std::unordered_set<std::string> set;
        Serializer::deserializeSet(data, offset, set);
        return set;
    }
    case 3:
    {
        std::unordered_map<std::string, std::string> map;
        Serializer::deserializeMap(data, offset, map);
        return map;
    }
    case 4:
    {
        ZSet zset;
        Serializer::deserializeZSet(data, offset, zset);
        return zset;
    }
    default:
        throw std::runtime_error("Invalid EyaValue index");
    }
}
/**
 * @brief 计算 std::string 的实际大小
 */
inline size_t calculateStringSize(const std::string &str)
{
    return str.size() + sizeof(std::string);
}
/**
 * @brief 估算 EyaValue 占用的内存大小
 */
inline size_t estimateEyaValueSize(const EyaValue &value)
{
    return std::visit([](auto &&arg) -> size_t
                      {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return arg.size() + sizeof(std::string);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            size_t total = sizeof(std::vector<std::string>);
            for (const auto& s : arg) {
                total += s.size() + sizeof(std::string);
            }
            return total;
        } else if constexpr (std::is_same_v<T, std::unordered_set<std::string>>) {
            size_t total = sizeof(std::unordered_set<std::string>);
            for (const auto& s : arg) {
                total += s.size() + sizeof(std::string) + HASH_COST; // 哈希表节点开销
            }
            return total;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::string>>) {
            size_t total = sizeof(std::unordered_map<std::string, std::string>);
            for (const auto& [k, v] : arg) {
                total += k.size() + v.size() + sizeof(std::string) * 2 + HASH_COST;
            }
            return total;
        } else if constexpr (std::is_same_v<T, ZSet>) {
            return arg.memory_usage();
        } else {
            return sizeof(T);
        } }, value);
}

#endif