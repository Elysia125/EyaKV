#ifndef PROTOCOL_H_
#define PROTOCOL_H_
#include "common/types/value.h"
#include "common/socket/protocol/protocol.h"
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
#undef ERROR // 避免与 ERROR 冲突
namespace ReturnCode
{
    constexpr int ERROR = 0;
    constexpr int SUCCESS = 1;
    constexpr int REDIRECT = 2; // 重定向
}
struct Response : public ProtocolBody
{
    int code_;
    ResponseData data_;
    std::string error_msg_;
    std::string request_id_;
    Response(int code, ResponseData data, const std::string &error_msg, const std::string &request_id)
        : code_(code), data_(data), error_msg_(error_msg), request_id_(request_id) {}
    Response() : code_(ReturnCode::ERROR), data_(std::monostate()), error_msg_(""), request_id_("") {}
    // 允许移动和拷贝
    Response(const Response &) = default;
    Response(Response &&) = default;
    Response &operator=(const Response &) = default;
    Response &operator=(Response &&) = default;
    static Response success(ResponseData data, const std::string &request_id = "")
    {
        return Response{ReturnCode::SUCCESS, data, "", request_id};
    }
    static Response success(bool success, const std::string &request_id = "")
    {
        return Response{ReturnCode::SUCCESS, std::string(success ? "1" : "0"), "", request_id};
    }
    static Response success(size_t data, const std::string &request_id = "")
    {
        return Response{ReturnCode::SUCCESS, std::to_string(data), "", request_id};
    }
    /**
     * @brief 创建一个包含错误信息的Result对象
     *
     * @param error_msg 错误描述信息
     * @return Response 包含错误信息的Result对象
     */
    static Response error(const std::string error_msg, const std::string &request_id = "")
    {
        return Response{ReturnCode::ERROR, std::monostate{}, error_msg, request_id};
    }

    static Response redirect(const std::string &redirect_addr, const std::string &request_id = "")
    {
        return Response{ReturnCode::REDIRECT, redirect_addr, "", request_id};
    }
    bool is_success() const
    {
        return code_ == ReturnCode::SUCCESS;
    }
    std::string serialize() const
    {
        std::string es;
        es.append(Serializer::serialize(request_id_));
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
    void deserialize(const char *data, size_t &offset)
    {
        this->request_id_ = Serializer::deserializeString(data, offset);
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
        this->code_ = code;
        this->data_ = rdata;
        this->error_msg_ = error_msg;
    }

    std::string to_string() const
    {
        std::stringstream ss;
        ss << request_id_ << ":";
        if (code_ == ReturnCode::SUCCESS)
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
            if(arg.empty()){
                ss<<"null";
            }else{
                for (const auto &s : arg)
                {
                    ss << s << ",";
                }
            }
        }
        else if constexpr (std::is_same_v<T, std::vector<std::pair<std::string, EyaValue>>>)
        {
            if(arg.empty()){
                ss<<"null";
            }else{
                for (const auto &p : arg)
                {
                    ss << "(" << p.first << ", " << ::to_string(p.second) << ") "<<",";
                }
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
        else if (code_ == ReturnCode::ERROR)
        {
            ss << error_msg_;
        }
        else if (code_ == ReturnCode::REDIRECT)
        {
            ss << "leader address:" << std::get<std::string>(data_);
        }
        std::string result = ss.str();
        if (result.back() == ',')
        {
            result.pop_back();
        }
        return result;
    }
};
enum RequestType
{
    NONE,         // 无操作
    AUTH,         // 权限认证
    COMMAND,      // 命令
    BATCH_COMMAND // 批量命令
};
struct Request : public ProtocolBody
{
    std::string id;                                            // 唯一标识
    RequestType type;                                          // 类型
    std::vector<std::pair<std::string, std::string>> commands; // 批处理命令
    std::string command;                                       // 单一命令
    std::string password;                                      // 认证密码
    std::string auth_key;                                      // 身份密钥
    Request() : type(RequestType::NONE) {}
    static Request auth(const std::string &request_id, const std::string &password)
    {
        Request req;
        req.type = RequestType::AUTH;
        req.id = request_id;
        req.password = password;
        return req;
    }
    static Request createCommand(const std::string &request_id, const std::string &cmd, const std::string &key)
    {
        Request req;
        req.type = RequestType::COMMAND;
        req.id = request_id;
        req.command = cmd;
        req.auth_key = key;
        return req;
    }
    static Request createBatchCommand(const std::string &batch_id, const std::vector<std::pair<std::string, std::string>> &cmds, const std::string &key)
    {
        Request req;
        req.type = RequestType::BATCH_COMMAND;
        req.id = batch_id;
        req.auth_key = key;
        req.commands = cmds;
        return req;
    }
    std::string serialize() const
    {
        std::string s;
        uint8_t request_type = static_cast<uint8_t>(this->type);
        s.append(Serializer::serialize(id));
        s.append(reinterpret_cast<const char *>(&request_type), sizeof(request_type));
        if (this->type == RequestType::COMMAND)
        {
            s.append(Serializer::serialize(command));
            s.append(Serializer::serialize(auth_key));
        }
        else if (this->type == RequestType::BATCH_COMMAND)
        {
            uint32_t size = htonl(static_cast<uint32_t>(commands.size()));
            s.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &cmd : commands)
            {
                s.append(Serializer::serialize(cmd.first));
                s.append(Serializer::serialize(cmd.second));
            }
            s.append(Serializer::serialize(auth_key));
        }
        else if (this->type == RequestType::AUTH)
        {
            s.append(Serializer::serialize(password));
        }
        return s;
    }
    void deserialize(const char *data, size_t &offset)
    {
        this->id = Serializer::deserializeString(data, offset);
        uint8_t type;
        std::memcpy(&type, data + offset, sizeof(type));
        offset += sizeof(type);
        this->type = static_cast<RequestType>(type);
        if (this->type == RequestType::COMMAND)
        {
            this->command = Serializer::deserializeString(data, offset);
            this->auth_key = Serializer::deserializeString(data, offset);
        }
        else if (this->type == RequestType::BATCH_COMMAND)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                std::string key = Serializer::deserializeString(data, offset);
                std::string value = Serializer::deserializeString(data, offset);
                this->commands.emplace_back(key, value);
            }
            this->auth_key = Serializer::deserializeString(data, offset);
        }
        else if (this->type == RequestType::AUTH)
        {
            this->password = Serializer::deserializeString(data, offset);
        }
    }
    std::string to_string() const
    {
        std::stringstream ss;
        ss << "Id: " << id << ", RequestType: " << (type == RequestType::NONE ? "NONE" : type == RequestType::AUTH  ? "AUTH"
                                                                                     : type == RequestType::COMMAND ? "COMMAND"
                                                                                                                    : "BATCH_COMMAND");
        if (type == RequestType::COMMAND)
        {
            ss << ", Command: " << command << ", AuthKey: " << auth_key;
        }
        else if (type == RequestType::BATCH_COMMAND)
        {
            ss << ", Commands: (";
            for (const auto &cmd : commands)
            {
                ss << cmd.first << ": " << cmd.second << ", ";
            }
            ss << "), AuthKey: " << auth_key;
        }
        else if (type == RequestType::AUTH)
        {
            ss << ", Password: " << password;
        }
        return ss.str();
    }
};

enum class ConnectionState
{
    WAITING, // 等待
    READY    // 就绪
};

inline std::string serialize(const std::pair<std::string, std::vector<std::pair<std::string, Response>>> &batch_responses)
{
    std::string s;
    s.append(Serializer::serialize(batch_responses.first));
    uint32_t size = htonl(static_cast<uint32_t>(batch_responses.second.size()));
    s.append(reinterpret_cast<const char *>(&size), sizeof(size));
    for (const auto &resp : batch_responses.second)
    {
        s.append(Serializer::serialize(resp.first));
        s.append(resp.second.serialize());
    }
    return s;
}

inline std::pair<std::string, std::vector<std::pair<std::string, Response>>> deserializeBatchResponse(const char *data, size_t &offset)
{
    std::string batch_id = Serializer::deserializeString(data, offset);
    std::vector<std::pair<std::string, Response>> batch_responses;
    uint32_t size;
    std::memcpy(&size, data + offset, sizeof(size));
    offset += sizeof(size);
    size = ntohl(size);
    for (uint32_t i = 0; i < size; ++i)
    {
        std::string key = Serializer::deserializeString(data, offset);
        Response resp;
        resp.deserialize(data, offset);
        batch_responses.emplace_back(key, resp);
    }
    return {batch_id, batch_responses};
}

inline std::string to_string(const std::pair<std::string, std::map<std::string, Response>> &batch_responses)
{
    std::stringstream ss;
    ss << "Batch ID: " << batch_responses.first << "\nResponses: (";
    for (const auto &resp : batch_responses.second)
    {
        ss << resp.first << ": " << resp.second.to_string() << "\n";
    }
    ss << ")";
    return ss.str();
}
#endif