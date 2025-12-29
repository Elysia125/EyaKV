#ifndef COMMON_H
#define COMMON_H

#include <string>
#include "zset.h"
#include "serializer.h"
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cstring>

#define HASH_COST 32
using EyaValue = std::variant<std::string,
                              std::deque<std::string>,
                              std::unordered_set<std::string>,
                              std::unordered_map<std::string, std::string>,
                              ZSet>;
using EData = std::variant<std::monostate,                                // 空状态
                           std::string,                                   // kExits,kZScore,kZRank,kZCard,kLGet,kLSize,kHGet,kZRemByRank,kZRemByScore,kRemove,kSet,kHSet,kHDel,kSAdd,kSRem,kZAdd,kZRem,kZIncrBy,kLPush,kRPush,kLPop,kRPop,kLPopN,kRPopN
                           std::vector<std::string>,                      // kHKeys,kHValues,kSMembers
                           std::vector<std::pair<std::string, EyaValue>>, // kRange,kHEntries,kZRangeByRank,kZRangeByScore,kLRange
                           EyaValue                                       // kGet
                           >;
struct Result
{
    EData data;
    std::string error_msg;
    static Result success(EData data, const std::string error_msg = "")
    {
        return Result{data, error_msg};
    }
    static Result error(const std::string error_msg)
    {
        return Result{std::monostate{}, error_msg};
    }
};
// LogType constants
namespace LogType
{
    // 通用
    constexpr uint8_t kExists = 0;
    constexpr uint8_t kRemove = kExists + 1;
    constexpr uint8_t kRange = kRemove + 1;
    constexpr uint8_t kExpire = kRange + 1;
    constexpr uint8_t kGet = kExpire + 1;
    // string
    constexpr uint8_t kSet = kGet + 1;
    // set
    constexpr uint8_t kSAdd = kSet + 1;
    constexpr uint8_t kSRem = kSAdd + 1;
    constexpr uint8_t kSMembers = kSRem + 1;
    // zset
    constexpr uint8_t kZAdd = kSMembers + 1;
    constexpr uint8_t kZRem = kZAdd + 1;
    constexpr uint8_t kZScore = kZRem + 1;
    constexpr uint8_t kZRank = kZScore + 1;
    constexpr uint8_t kZCard = kZRank + 1;
    constexpr uint8_t kZIncrBy = kZCard + 1;
    constexpr uint8_t kZRangeByRank = kZIncrBy + 1;
    constexpr uint8_t kZRangeByScore = kZRangeByRank + 1;
    constexpr uint8_t kZRemByRank = kZRangeByScore + 1;
    constexpr uint8_t kZRemByScore = kZRemByRank + 1;
    // list
    constexpr uint8_t kLPush = kZRemByScore + 1;
    constexpr uint8_t kLPop = kLPush + 1;
    constexpr uint8_t kRPush = kLPop + 1;
    constexpr uint8_t kRPop = kRPush + 1;
    constexpr uint8_t kLRange = kRPop + 1;
    constexpr uint8_t kLGet = kLRange + 1;
    constexpr uint8_t kLSize = kLGet + 1;
    constexpr uint8_t kLPopN = kLSize + 1;
    constexpr uint8_t kRPopN = kLPopN + 1;
    // map
    constexpr uint8_t kHSet = kRPopN + 1;
    constexpr uint8_t kHGet = kHSet + 1;
    constexpr uint8_t kHDel = kHGet + 1;
    constexpr uint8_t kHKeys = kHDel + 1;
    constexpr uint8_t kHValues = kHKeys + 1;
    constexpr uint8_t kHEntries = kHValues + 1;
}

struct EValue
{
    EyaValue value;
    bool deleted;
    size_t expire_time = 0; // 0代表不会过期
    EValue() : value(), deleted(false) {}
    EValue(const EyaValue &value) : value(value), deleted(false) {}
    EValue(const EyaValue &value, bool deleted) : value(value), deleted(deleted) {}

    bool is_deleted() const
    {
        return deleted;
    }
    bool is_expired() const
    {
        return expire_time > 0 && expire_time < std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

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
        std::deque<std::string> dq;
        Serializer::deserializeDeque(data, offset, dq);
        return dq;
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

inline std::string serialize(const EValue &value)
{
    std::string result;
    result.append(reinterpret_cast<const char *>(&value.deleted), sizeof(value.deleted));
    result.append(reinterpret_cast<const char *>(&value.expire_time), sizeof(value.expire_time));
    result.append(serialize_eya_value(value.value));
    return result;
}

inline EValue deserialize(const char *data, size_t &offset)
{
    EValue result;
    std::memcpy(&result.deleted, data + offset, sizeof(result.deleted));
    offset += sizeof(result.deleted);
    std::memcpy(&result.expire_time, data + offset, sizeof(result.expire_time));
    offset += sizeof(result.expire_time);
    result.value = deserialize_eya_value(data, offset);
    return result;
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

inline size_t estimateEValueSize(const EValue &value)
{
    return sizeof(EValue) + estimateEyaValueSize(value.value);
}
inline std::string serialize(const Result &result)
{
    std::string es;
    es.append(std::visit([](auto &&arg) -> std::string
                         {
                    std::string s;
                    s.append(reinterpret_cast<const char *>(&arg.index()), sizeof(arg.index()));
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>)
        {
            s.append(reinterpret_cast<const char *>(&arg.size()), sizeof(arg.size()));
            for (const auto &[k, v] : arg)
            {
                s.append(k);
                s.append(serialize_eya_value(v));
            }
        }else if constexpr (std::is_same_v<T, EyaValue>)
        {
            s.append(serialize_eya_value(arg));
        }
        else if constexpr (!std::is_same_v<T, std::monostate>){
            s.append(Serializer::serialize(arg));
        }
        return s; }, result.data));
    es.append(Serializer::serialize(result.error_msg));
    return es;
}

inline Result deserializeResult(const char *data, size_t &offset)
{
    uint8_t type_index;
    std::memcpy(&type_index, data + offset, sizeof(type_index));
    offset += sizeof(type_index);
    size_t index = type_index;
    Result result;
    switch (index)
    {
    case 0:
    {
        result.data = std::monostate();
        break;
    }
    case 1:
    {
        result.data = Serializer::deserializeString(data, offset);
        break;
    }
    case 2:
    {
        std::vector<std::string> v;
        Serializer::deserializeVector(data, offset, v);
        result.data = v;
        break;
    }
    case 3:
    {
        std::vector<std::pair<std::string, EyaValue>> v;
        size_t size;
        std::memcpy(&size, data + offset, sizeof(size));
        offset += sizeof(size);
        for (size_t i = 0; i < size; i++)
        {
            std::string key = Serializer::deserializeString(data, offset);
            EyaValue value = deserialize_eya_value(data, offset);
            v.emplace_back(key, value);
        }
        result.data = v;
        break;
    }
    case 4:
    {
        EyaValue value = deserialize_eya_value(data, offset);
        result.data = value;
        break;
    }
    default:
        throw std::runtime_error("Invalid Result index");
    }
    result.error_msg = Serializer::deserializeString(data, offset);
    return result;
}
inline std::string to_string(const EyaValue &value)
{
    std::visit([](auto &&arg)
               {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>)
        {
            return arg;
        }
        else if constexpr (std::is_same_v<T, std::deque<std::string>>)
        {
            std::string s;
            s+="[";
            for (const auto &str : arg)
            {
                s += str + ",";
            }
            s.pop_back();
            s+="]";
            return s;
        }
        else if constexpr (std::is_same_v<T, std::unordered_set<std::string>>)
        {
            std::string s;
            s+="(";
            for (const auto &str : arg)
            {
                s += str + ",";
            }
            s.pop_back();
            s+=")";
            return s;
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::string>>)
        {
            std::string s;
            s+="{";
            for (const auto &[key, value] : arg)
            {
                s += key + ": " + value + ", ";
            }
            s.pop_back();
            s+="}";
            return s;
        }
        else if constexpr (std::is_same_v<T, ZSet>)
        {
            std::string s;
            s+="zset(";
            arg.for_each([&s](const std::string&score,const std::string&member){
                s += member + "=" + score + ", ";
            });
            if (s.back() == ',') {
                s.pop_back();
            }
            s+=")";
            return s;
        }
        else
        {
            return "unknown type";
        } }, value);
}
inline void printResult(const Result &result)
{
    if (!std::holds_alternative<std::monostate>(result.data))
    {
        std::visit([](auto &&arg)
                   {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>)
        {
            std::cout << arg << std::endl;
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        {
            for (const auto &s : arg)
            {
                std::cout << s << ",";
            }
            std::cout << std::endl;
        }
        else if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>>)
        {
            for (const auto &p : arg)
            {
                std::cout << "(" << p.first << ", " << to_string(p.second) << ") ";
            }
            std::cout << std::endl;
        }else if constexpr (std::is_same_v<T, EyaValue>)
        {
            std::cout << to_string(arg) << std::endl;
        }
        else
        {
            std::cout << "unknown type" << std::endl;
        } }, result.data);
    }
    else if (!result.error_msg.empty())
    {
        std::cout << result.error_msg << std::endl;
    }
    else
    {
        std::cout << "null" << std::endl;
    }
}
#endif