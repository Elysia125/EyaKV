#ifndef PROTOCOL_H_
#define PROTOCOL_H_
#include "common/common.h"
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
using ResponseData = std::variant<std::monostate,                                // 空状态
                                  std::string,                                   // kExits,kZScore,kZRank,kZCard,kLGet,kLSize,kHGet,kZRemByRank,kZRemByScore,kRemove,kSet,kHSet,kHDel,kSAdd,kSRem,kZAdd,kZRem,kZIncrBy,kLPush,kRPush,kLPop,kRPop,kLPopN,kRPopN
                                  std::vector<std::string>,                      // kHKeys,kHValues,kSMembers
                                  std::vector<std::pair<std::string, EyaValue>>, // kRange,kHEntries,kZRangeByRank,kZRangeByScore,kLRange
                                  EyaValue                                       // kGet
                                  >;
enum HeaderType
{
    REQUEST = 0,
    RESPONSE = 1
};
struct Header
{
    uint32_t magic_number = htonl(0xEA1314);
    uint8_t version = 1;
    HeaderType type;
    uint32_t length;
    Header(HeaderType type, uint32_t length, uint8_t version = 1) : type(type), length(htonl(length)), version(version) {}
    std::string serialize() const
    {
        uint8_t header_type = static_cast<uint8_t>(type);
        std::string result;
        result.append(reinterpret_cast<const char *>(&magic_number), sizeof(magic_number));
        result.append(reinterpret_cast<const char *>(&version), sizeof(version));
        result.append(reinterpret_cast<const char *>(&header_type), sizeof(header_type));
        result.append(reinterpret_cast<const char *>(&length), sizeof(length));
        return result;
    }

    static Header deserialize(const char *data, size_t &offset)
    {
        uint32_t magic_number;
        std::memcpy(&magic_number, data + offset, sizeof(magic_number));
        offset += sizeof(magic_number);
        if (ntohl(magic_number) != 0xEA1314)
        {
            throw std::runtime_error("Invalid magic number");
        }
        uint8_t version;
        std::memcpy(&version, data + offset, sizeof(version));
        offset += sizeof(version);
        uint8_t type;
        std::memcpy(&type, data + offset, sizeof(type));
        offset += sizeof(type);
        uint32_t length;
        std::memcpy(&length, data + offset, sizeof(length));
        offset += sizeof(length);
        return Header(static_cast<HeaderType>(type), ntohl(length), version);
    }
};

class Response
{
private:
    int code_;
    ResponseData data_;
    std::string error_msg_;

public:
    Response(int code, ResponseData data, const std::string &error_msg)
        : code_(code), data_(data), error_msg_(error_msg) {}
    static Response success(ResponseData data, const std::string error_msg = "")
    {
        return Response{1, data, error_msg};
    }
    static Response success(bool success, const std::string error_msg = "")
    {
        return Response{1, std::string(success ? "1" : "0"), error_msg};
    }
    static Response success(size_t data, const std::string error_msg = "")
    {
        return Response{1, std::to_string(data), error_msg};
    }
    /**
     * @brief 创建一个包含错误信息的Result对象
     *
     * @param error_msg 错误描述信息
     * @return Response 包含错误信息的Result对象
     */
    static Response error(const std::string error_msg)
    {
        return Response{0, std::monostate{}, error_msg};
    }
    std::string serialize() const
    {
        std::string es;
        // Serialize code
        int code = htonl(code_);
        es.append(reinterpret_cast<const char *>(&code), sizeof(code));
        // Serialize data with proper type handling
        uint8_t type_index = static_cast<uint8_t>(data_.index());
        es.append(reinterpret_cast<const char *>(&type_index), sizeof(type_index));
        es.append(std::visit([](auto &&arg) -> std::string
                             {
                    std::string s;
                    using T = std::decay_t<decltype(arg)>;
                    // Serialize value based on type
        if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>>)
        {
            uint32_t vec_size = htonl(static_cast<uint32_t>(arg.size()));
            s.append(reinterpret_cast<const char *>(&vec_size), sizeof(vec_size));
            for (const auto &[k, v] : arg)
            {
                s.append(Serializer::serialize(k));
                s.append(serialize_eya_value(v));
            }
        } else if constexpr (std::is_same_v<T, EyaValue>)
        {
            s.append(serialize_eya_value(arg));
        }
        else if constexpr (!std::is_same_v<T, std::monostate>){
            s.append(Serializer::serialize(arg));
        }
        return s; }, data_));
        es.append(Serializer::serialize(error_msg_));
        return es;
    }
    static Response deserializeResponse(const char *data, size_t &offset)
    {
        int code;
        std::memcpy(&code, data + offset, sizeof(code));
        offset += sizeof(code);
        code = ntohl(code);
        uint8_t type_index;
        std::memcpy(&type_index, data + offset, sizeof(type_index));
        offset += sizeof(type_index);
        size_t index = type_index;
        ResponseData rdata;
        switch (index)
        {
        case 0:
        {
            rdata = std::monostate();
            break;
        }
        case 1:
        {
            rdata = Serializer::deserializeString(data, offset);
            break;
        }
        case 2:
        {
            std::vector<std::string> v;
            Serializer::deserializeVector(data, offset, v);
            rdata = v;
            break;
        }
        case 3:
        {
            std::vector<std::pair<std::string, EyaValue>> v;
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                std::string key = Serializer::deserializeString(data, offset);
                EyaValue value = deserialize_eya_value(data, offset);
                v.emplace_back(key, value);
            }
            rdata = v;
            break;
        }
        case 4:
        {
            EyaValue value = deserialize_eya_value(data, offset);
            rdata = value;
            break;
        }
        default:
            throw std::runtime_error("Invalid Response index");
        }
        std::string error_msg = Serializer::deserializeString(data, offset);
        return Response(code, rdata, error_msg);
    }

    std::string to_string()
    {
        std::stringstream ss;
        if (code_ != 1)
        {
            if (!std::holds_alternative<std::monostate>(data_))
            {
                std::visit([&ss](auto &&arg)
                           {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>)
        {
            ss << arg;
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        {
            for (const auto &s : arg)
            {
                ss << s << ",";
            }
        }
        else if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>>)
        {
            for (const auto &p : arg)
            {
                ss << "(" << p.first << ", " << ::to_string(p.second) << ") ";
            }
        }else if constexpr (std::is_same_v<T, EyaValue>)
        {
            ss << ::to_string(arg);
        }
        else
        {
            ss << "unknown type";
        } }, data_);
            }
            else
            {
                ss << "null";
            }
        }
        else if (code_ == 0)
        {
            ss << error_msg_;
        }
        return ss.str();
    }
};
enum RequestType
{
    AUTH,   // 权限认证
    COMMAND // 命令
};
class Request
{
private:
    RequestType type;
    std::string command;

public:
    Request(RequestType t, const std::string &cmd) : type(t), command(cmd) {}
    static Request auth(const std::string &password)
    {
        return Request(RequestType::AUTH, password);
    }
    static Request createCommand(const std::string &cmd)
    {
        return Request(RequestType::COMMAND, cmd);
    }
    std::string serialize() const
    {
        std::string s;
        uint8_t request_type = static_cast<uint8_t>(this->type);
        s.append(reinterpret_cast<const char *>(&request_type), sizeof(request_type));
        s.append(Serializer::serialize(command));
        return s;
    }
    static Request deserializeRequest(const char *data, size_t &offset)
    {
        uint8_t type;
        std::memcpy(&type, data + offset, sizeof(type));
        offset += sizeof(type);
        std::string command = Serializer::deserializeString(data, offset);
        return Request(static_cast<RequestType>(type), command);
    }
    std::string to_string()
    {
        std::stringstream ss;
        ss << "RequestType: " << type << ", Command: " << command;
        return ss.str();
    }
};
#endif