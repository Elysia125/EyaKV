#ifndef COMMON_H
#define COMMON_H

#include <string>
#include "common/ds/zset.h"
#include "common/serialization/serializer.h"
#include "common/types/key_encoder.h"
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cstring>
#include <sstream>

#define HASH_COST 32

/**
 * @brief EyaType 定义数据底层逻辑类型
 */
enum class EyaType : uint8_t
{
    kHash = 1,
    kSet = 2,
    kZSet = 3,
    kList = 4
};

using EmbeddedValue = std::variant<std::monostate, std::unordered_map<std::string, std::string>, std::unordered_set<std::string>, std::deque<std::string>, ZSet>;

inline std::string serialize(const EmbeddedValue &value)
{
    std::string res;
    uint32_t index = value.index();
    res.append(reinterpret_cast<const char *>(&index), sizeof(index));
    switch (index)
    {
    case 0:
        break;
    case 1:
        res = Serializer::serialize(std::get<std::unordered_map<std::string, std::string>>(value));
        break;
    case 2:
        res = Serializer::serialize(std::get<std::unordered_set<std::string>>(value));
        break;
    case 3:
        res = Serializer::serialize(std::get<std::deque<std::string>>(value));
        break;
    case 4:
        res = Serializer::serialize(std::get<ZSet>(value));
        break;
    default:
        throw std::runtime_error("unsupported EmbeddedValue type");
    }
    return res;
}

inline EmbeddedValue deserialize_embedded(const char *data, size_t &offset)
{
    uint32_t index;
    std::memcpy(&index, data + offset, sizeof(index));
    offset += sizeof(index);
    switch (index)
    {
    case 0:
        return EmbeddedValue(std::monostate{});
    case 1:
    {
        std::unordered_map<std::string, std::string> map;
        Serializer::deserializeMap(data, offset, map);
        return EmbeddedValue(std::move(map));
    }
    case 2:
    {
        std::unordered_set<std::string> set;
        Serializer::deserializeSet(data, offset, set);
        return EmbeddedValue(std::move(set));
    }
    case 3:
    {
        std::deque<std::string> deque;
        Serializer::deserializeDeque(data, offset, deque);
        return EmbeddedValue(std::move(deque));
    }
    case 4:
    {
        ZSet zset;
        Serializer::deserializeZSet(data, offset, zset);
        return EmbeddedValue(std::move(zset));
    }
    default:
        throw std::runtime_error("unsupported EmbeddedValue type");
    }
}

/**
 * @brief Metadata 用于存放底层复杂结构的头部描述与宏观属性
 */
struct Metadata
{
    uint8_t type;               // 对应的 EyaType
    uint64_t version;           // 全局单调递增版本号或时间戳，用于隔离旧数据
    uint32_t size;              // 当前包含子元素的个数
    uint64_t head_seq = 0;      // 主要给 List(Deque) 用，表示队头Seq
    uint64_t tail_seq = 0;      // 主要给 List(Deque) 用，表示队尾Seq
    EmbeddedValue embeded_data; // 内嵌数据字段，用于存放数据量少时的直接值（如小Hash）
    Metadata(uint8_t t, uint64_t v) : type(t), version(v), size(0) {}
    Metadata() : type(0), version(0), size(0) {}

    /**
     * @brief 序列化元数据对象为定长字符串
     */
    std::string serialize() const
    {
        std::string res;
        res.push_back(type);

        uint64_t v_be = EncodeUtil::encode_u64_be(version);
        res.append(reinterpret_cast<const char *>(&v_be), sizeof(v_be));

        uint32_t s_be = htonl(size);
        res.append(reinterpret_cast<const char *>(&s_be), sizeof(s_be));

        uint64_t hs_be = EncodeUtil::encode_u64_be(head_seq);
        res.append(reinterpret_cast<const char *>(&hs_be), sizeof(hs_be));

        uint64_t ts_be = EncodeUtil::encode_u64_be(tail_seq);
        res.append(reinterpret_cast<const char *>(&ts_be), sizeof(ts_be));
        res.append(::serialize(embeded_data));
        return res;
    }
    
    /**
     * @brief 反序列化给定数据流填充回元数据结构
     */
    void deserialize(const char *data, size_t &offset)
    {
        uint8_t te;
        std::memcpy(&type, data + offset, sizeof(type));
        type = te;
        offset += sizeof(type);

        uint64_t v_be;
        std::memcpy(&v_be, data + offset, sizeof(v_be));
        version = EncodeUtil::decode_u64_be(v_be);
        offset += 8;

        uint32_t s_be;
        std::memcpy(&s_be, data + offset, sizeof(s_be));
        size = ntohl(s_be);
        offset += 4;

        uint64_t hs_be;
        std::memcpy(&hs_be, data + offset, sizeof(hs_be));
        head_seq = EncodeUtil::decode_u64_be(hs_be);
        offset += 8;

        uint64_t ts_be;
        std::memcpy(&ts_be, data + offset, sizeof(ts_be));
        tail_seq = EncodeUtil::decode_u64_be(ts_be);
        offset += 8;

        uint32_t id_be;
        std::memcpy(&id_be, data + offset, sizeof(id_be));
        id_be = ntohl(id_be);
        offset += 4;
        embeded_data = deserialize_embedded(data, offset);
    }

    /**
     * @brief 生成一个全新的单调自增 Version，主要依赖系统墙钟纳秒，以确保全局隔离性
     */
    static uint64_t generate_version()
    {
        return std::chrono::time_point_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now())
            .time_since_epoch()
            .count();
    }

    std::string to_string() const
    {
        std::stringstream ss;
        switch (type)
        {
        case static_cast<uint8_t>(EyaType::kHash):
            ss << "Type: Hash, ";
            break;
        case static_cast<uint8_t>(EyaType::kSet):
            ss << "Type: Set, ";
            break;
        case static_cast<uint8_t>(EyaType::kZSet):
            ss << "Type: ZSet, ";
            break;
        case static_cast<uint8_t>(EyaType::kList):
            ss << "Type: List, ";
            break;
        default:
            break;
        }
        ss << "Size: " << size;
        return ss.str();
    }
};

struct ListElement
{
    uint64_t seq;
    std::deque<std::string> values;
    ListElement() = default;
    ListElement(uint64_t s) : seq(s) {}
    ListElement(ListElement &&other) noexcept : seq(other.seq), values(std::move(other.values)) {}
    ListElement &operator=(ListElement &&other) noexcept
    {
        if (this != &other)
        {
            seq = other.seq;
            values = std::move(other.values);
        }
        return *this;
    }
    ListElement(const ListElement &other) = default;
    ListElement &operator=(const ListElement &other) = default;

    std::string serialize() const
    {
        std::string res;
        uint64_t seq_be = EncodeUtil::encode_u64_be(seq);
        res.append(reinterpret_cast<const char *>(&seq_be), sizeof(seq_be));
        uint32_t size_be = htonl(static_cast<uint32_t>(values.size()));
        res.append(reinterpret_cast<const char *>(&size_be), sizeof(size_be));
        for (const auto &val : values)
        {
            res.append(Serializer::serialize(val));
        }
        return res;
    }
    void deserialize(const char *data, size_t &offset)
    {
        uint64_t seq_be;
        std::memcpy(&seq_be, data + offset, sizeof(seq_be));
        seq = EncodeUtil::decode_u64_be(seq_be);
        offset += 8;

        uint32_t size_be;
        std::memcpy(&size_be, data + offset, sizeof(size_be));
        size_t size = ntohl(size_be);
        offset += 4;

        values.clear();
        for (size_t i = 0; i < size; ++i)
        {
            values.push_back(Serializer::deserializeString(data, offset));
        }
    }
};

// 保留 EyaValue 作为命令解析器组装/格式化向外的统一返回封装。
// 底层持久化不再直接序列化整个 variant 对象。
using EyaValue = std::variant<std::string,
                              std::deque<std::string>,
                              std::unordered_set<std::string>,
                              std::unordered_map<std::string, std::string>,
                              ZSet,
                              Metadata,
                              ListElement>;

inline std::string serialize_eya_value(const EyaValue &value)
{
    std::string result;
    // 先写入类型索引（1字节）
    uint8_t type_index = static_cast<uint8_t>(value.index());
    result.append(reinterpret_cast<const char *>(&type_index), sizeof(type_index));
    // 再写入序列化后的值
    result.append(std::visit([](auto &&arg) -> std::string
                             { 
                                 using T = std::decay_t<decltype(arg)>;
                                 if constexpr (std::is_same_v<T, Metadata> || std::is_same_v<T, ListElement>) {
                                     return arg.serialize();
                                 } else {
                                     return Serializer::serialize(arg); 
                                 } }, value));
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
    case 5:
    {
        Metadata meta;
        meta.deserialize(data, offset);
        return meta;
    }
    case 6:
    {
        ListElement elem;
        elem.deserialize(data, offset);
        return elem;
    }
    default:
        throw std::runtime_error("Invalid EyaValue index");
    }
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
        } else if constexpr (std::is_same_v<T, std::deque<std::string>>) {
            size_t total = sizeof(std::deque<std::string>);
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
        } else if constexpr (std::is_same_v<T, Metadata>) {
            return sizeof(Metadata);
        } else if constexpr (std::is_same_v<T, ListElement>) {
            size_t total = sizeof(ListElement);
            for (const auto &elem : arg.values)
            {
                total += elem.size() + sizeof(std::string);
            }
            return total;
        } else {
            return sizeof(T);
        } }, value);
}

inline std::string to_string(const EyaValue &value)
{
    return std::visit([](auto &&arg)
                      {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>)
        {
            return arg;
        }
        else if constexpr (std::is_same_v<T, std::deque<std::string>>)
        {
            std::stringstream ss;
            ss<<"[";
            for (const auto &str : arg)
            {
                ss<<str << ",";
            }
            std::string s=ss.str();
            if(s.back()==',') { s.pop_back();}
            s+="]";
            return s;
        }
        else if constexpr (std::is_same_v<T, std::unordered_set<std::string>>)
        {
            std::stringstream ss;
            ss<<"(";
            for (const auto &str : arg)
            {
                ss<<str << ",";
            }
            std::string s=ss.str();
            if(s.back()==',') { s.pop_back();}
            s+=")";
            return s;
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::string>>)
        {
            std::stringstream ss;
            ss<<"{";
            for (const auto &[key, value] : arg)
            {
                ss<<key <<": " << value << ", ";
            }
            std::string s=ss.str();
            if(s.back()==',') { s.pop_back();}
            s+="}";
            return s;
        }
        else if constexpr (std::is_same_v<T, ZSet>)
        {
            std::stringstream ss;
            ss<<"zset(";
            arg.for_each([&ss](const std::string&member,const double score){
                ss << member << "=" << score << ", ";
            });
            std::string s = ss.str();
            if (s.back() == ',') {
                s.pop_back();
            }
            s+=")";
            return s;
        }
        else if constexpr (std::is_same_v<T, Metadata>)
        {
            return arg.to_string();
        }
        else if constexpr (std::is_same_v<T, ListElement>)
        {
            std::stringstream ss;
            ss<<"ListElement(seq="<<arg.seq<<", values=[";
            for (const auto &val : arg.values)
            {
                ss << val << ", ";
            }
            std::string s = ss.str();
            if (s.back() == ',') {
                s.pop_back();
            }
            s+="])";
            return s;
        }
        else
        {
            return "unknown type";
        } }, value);
}

#endif