#ifndef PROTOCOL_H_
#define PROTOCOL_H_
#include "common/common.h"

using ResponseData = std::variant<std::monostate,                                // 空状态
                                  std::string,                                   // kExits,kZScore,kZRank,kZCard,kLGet,kLSize,kHGet,kZRemByRank,kZRemByScore,kRemove,kSet,kHSet,kHDel,kSAdd,kSRem,kZAdd,kZRem,kZIncrBy,kLPush,kRPush,kLPop,kRPop,kLPopN,kRPopN
                                  std::vector<std::string>,                      // kHKeys,kHValues,kSMembers
                                  std::vector<std::pair<std::string, EyaValue>>, // kRange,kHEntries,kZRangeByRank,kZRangeByScore,kLRange
                                  EyaValue                                       // kGet
                                  >;
class Response
{
private:
    int code;
    ResponseData data;
    std::string error_msg;

public:
    Response(int code, ResponseData data, const std::string &error_msg)
        : code(code), data(data), error_msg(error_msg) {}
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
    std::string serialize()
    {
        std::string es;
        // Serialize code
        es.append(reinterpret_cast<const char *>(&code), sizeof(code));
        // Serialize data with proper type handling
        uint8_t type_index = static_cast<uint8_t>(data.index());
        es.append(reinterpret_cast<const char *>(&type_index), sizeof(type_index));
        es.append(std::visit([](auto &&arg) -> std::string
                             {
                    std::string s;
                    using T = std::decay_t<decltype(arg)>;
                    // Serialize value based on type
        if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>>)
        {
            size_t vec_size = arg.size();
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
        return s; }, data));
        es.append(Serializer::serialize(error_msg));
        return es;
    }
    static Response deserializeResponse(const char *data, size_t &offset)
    {
        int code;
        std::memcpy(&code, data + offset, sizeof(code));
        offset += sizeof(code);
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
            size_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            for (size_t i = 0; i < size; i++)
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
        if (code != 1)
        {
            if (!std::holds_alternative<std::monostate>(data))
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
        } }, data);
            }
            else
            {
                ss << "null";
            }
        }
        else if (code == 0)
        {
            ss << error_msg;
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
    static Request auth(const std::string &username, const std::string &password)
    {
        return Request(RequestType::AUTH, username + " " + password);
    }
    static Request command(const std::string &cmd)
    {
        return Request(RequestType::COMMAND, cmd);
    }
    std::string serialize()
    {
        std::string s;
        s.append(reinterpret_cast<const char *>(&type), sizeof(type));
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