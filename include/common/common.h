#include "zset.h"
#include "serializer.h"
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring>
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
    result.append(reinterpret_cast<const char*>(&type_index), sizeof(type_index));
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