#ifndef RAFT_PROTOCOL_H_
#define RAFT_PROTOCOL_H_
#include <cstdint>
#include <string>
#include <cstring>
#include <set>
#include <optional>
#include <bit>
#include "common/socket/protocol/protocol.h"
#include "common/serialization/serializer.h"
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
//  日志缓冲区配置
#define RAFT_LOG_BUFFER_SIZE 8192 // 环形缓冲区大小

using Serializer = EyaKV::Serializer;

//  RPC 消息类型
enum RaftMessageType
{
    HEART_BEAT = 0,              // 心跳消息
    REQUEST_VOTE = 1,            // 请求投票
    REQUEST_VOTE_RESPONSE = 2,   // 请求投票响应
    APPEND_ENTRIES = 3,          // 日志追加
    APPEND_ENTRIES_RESPONSE = 4, // 日志追加响应
    NEW_CONNECTION = 5,          // 新连接
    DATA_SNAPSHOT_RESPONSE = 6,  // 数据快照响应
    NEW_MASTER = 7,              // 新主节点选举成功消息
    NEW_SLAVER = 8,              // 新从节点消息
};
//  日志条目结构体
struct LogEntry
{
    uint32_t term;   // 日志所属任期
    uint32_t index;  // 日志索引（全局唯一）
    std::string cmd; // 客户端命令

    LogEntry() : term(0), index(0) {}
    LogEntry(uint32_t t, uint32_t i, const std::string &c) : term(t), index(i), cmd(c) {}

    // 序列化
    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint32_t net_index = htonl(index);
        uint32_t cmd_len = htonl(static_cast<uint32_t>(cmd.size()));
        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&net_index), sizeof(net_index));
        result.append(reinterpret_cast<const char *>(&cmd_len), sizeof(cmd_len));
        result.append(cmd);
        return result;
    }

    // 反序列化
    static LogEntry deserialize(const char *data, size_t &offset)
    {
        LogEntry entry;
        uint32_t net_term, net_index, cmd_len;
        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        entry.term = ntohl(net_term);
        std::memcpy(&net_index, data + offset, sizeof(net_index));
        offset += sizeof(net_index);
        entry.index = ntohl(net_index);
        std::memcpy(&cmd_len, data + offset, sizeof(cmd_len));
        offset += sizeof(cmd_len);
        cmd_len = ntohl(cmd_len);
        entry.cmd.assign(data + offset, cmd_len);
        offset += cmd_len;
        return entry;
    }

    bool operator==(const LogEntry &other) const
    {
        return term == other.term && index == other.index && cmd == other.cmd;
    }
};

namespace std
{
    template <>
    struct hash<LogEntry>
    {
        size_t operator()(const LogEntry &entry) const
        {
            return std::hash<uint32_t>()(entry.term) ^ std::hash<uint32_t>()(entry.index);
        }
    };
}
struct NewConnection
{
    bool is_reconnect;   // 是否为重新连接
    uint32_t last_index; // 日志的最后一个索引
    NewConnection(bool is_reconnect, uint32_t last_index) : is_reconnect(is_reconnect), last_index(last_index) {}
    NewConnection() : is_reconnect(false), last_index(0) {}
    std::string serialize() const
    {
        std::string result;
        uint8_t net_is_reconnect = static_cast<uint8_t>(is_reconnect);
        uint32_t net_last_index = htonl(last_index);
        result.append(reinterpret_cast<const char *>(&net_is_reconnect), sizeof(net_is_reconnect));
        result.append(reinterpret_cast<const char *>(&net_last_index), sizeof(net_last_index));
        return result;
    }

    static NewConnection deserialize(const char *data, size_t &offset)
    {
        uint8_t net_is_reconnect;
        uint32_t net_last_index;
        std::memcpy(&net_is_reconnect, data + offset, sizeof(net_is_reconnect));
        offset += sizeof(net_is_reconnect);
        std::memcpy(&net_last_index, data + offset, sizeof(net_last_index));
        offset += sizeof(net_last_index);
        return NewConnection(net_is_reconnect != 0, ntohl(net_last_index));
    }
};

struct NewSlaver
{
    std::string ip;
    uint16_t port;
    NewSlaver() = default;
    NewSlaver(std::string ip, uint16_t port) : ip(ip), port(port) {}
    std::string serialize() const
    {
        std::string result;
        uint16_t net_port = htons(port);
        result.append(Serializer::serialize(ip));
        result.append(reinterpret_cast<const char *>(&net_port), sizeof(net_port));
        return result;
    }

    static NewSlaver deserialize(const char *data, size_t &offset)
    {
        std::string ip = Serializer::deserializeString(data, offset);
        uint16_t net_port;
        std::memcpy(&net_port, data + offset, sizeof(net_port));
        offset += sizeof(net_port);
        return NewSlaver(ip, ntohs(net_port));
    }
};

// 心跳消息数据
struct HeartBeatData
{
    uint32_t leader_commit; // 领导者的提交索引

    HeartBeatData(uint32_t commit = 0) : leader_commit(commit) {}

    std::string serialize() const
    {
        std::string result;
        uint32_t net_leader_commit = htonl(leader_commit);
        result.append(reinterpret_cast<const char *>(&net_leader_commit), sizeof(net_leader_commit));
        return result;
    }

    static HeartBeatData deserialize(const char *data, size_t &offset)
    {
        uint32_t net_leader_commit;
        std::memcpy(&net_leader_commit, data + offset, sizeof(net_leader_commit));
        offset += sizeof(net_leader_commit);
        return HeartBeatData(ntohl(net_leader_commit));
    }
};

// 请求投票消息数据
struct RequestVoteData
{
    uint32_t last_log_index; // 候选人最后一条日志的索引

    RequestVoteData(uint32_t last_idx = 0) : last_log_index(last_idx) {}

    std::string serialize() const
    {
        std::string result;
        uint32_t net_last_log_index = htonl(last_log_index);
        result.append(reinterpret_cast<const char *>(&net_last_log_index), sizeof(net_last_log_index));
        return result;
    }

    static RequestVoteData deserialize(const char *data, size_t &offset)
    {
        uint32_t net_last_log_index;
        std::memcpy(&net_last_log_index, data + offset, sizeof(net_last_log_index));
        offset += sizeof(net_last_log_index);
        return RequestVoteData(ntohl(net_last_log_index));
    }
};

// 请求投票响应消息数据
struct RequestVoteResponseData
{
    bool vote_granted; // 是否投票

    RequestVoteResponseData(bool granted = false) : vote_granted(granted) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_vote_granted = static_cast<uint8_t>(vote_granted ? 1 : 0);
        result.append(reinterpret_cast<const char *>(&net_vote_granted), sizeof(net_vote_granted));
        return result;
    }

    static RequestVoteResponseData deserialize(const char *data, size_t &offset)
    {
        uint8_t net_vote_granted;
        std::memcpy(&net_vote_granted, data + offset, sizeof(net_vote_granted));
        offset += sizeof(net_vote_granted);
        return RequestVoteResponseData(net_vote_granted != 0);
    }
};

// 日志追加消息数据
struct AppendEntriesData
{
    uint32_t prev_log_index;                   // 前一个日志的索引
    uint32_t prev_log_term;                    // 前一个日志的任期
    std::optional<std::set<LogEntry>> entries; // 要追加的日志条目
    uint32_t leader_commit;                    // 领导者的提交索引

    AppendEntriesData(uint32_t prev_idx = 0, uint32_t prev_term = 0,
                      uint32_t commit = 0)
        : prev_log_index(prev_idx), prev_log_term(prev_term), leader_commit(commit) {}

    AppendEntriesData(uint32_t prev_idx, uint32_t prev_term,
                      const std::set<LogEntry> &ents, uint32_t commit)
        : prev_log_index(prev_idx), prev_log_term(prev_term), entries(ents), leader_commit(commit) {}

    std::string serialize() const
    {
        std::string result;
        uint32_t net_prev_log_index = htonl(prev_log_index);
        uint32_t net_prev_log_term = htonl(prev_log_term);
        uint32_t net_leader_commit = htonl(leader_commit);

        result.append(reinterpret_cast<const char *>(&net_prev_log_index), sizeof(net_prev_log_index));
        result.append(reinterpret_cast<const char *>(&net_prev_log_term), sizeof(net_prev_log_term));

        if (entries)
        {
            uint32_t entry_count = htonl(static_cast<uint32_t>(entries->size()));
            result.append(reinterpret_cast<const char *>(&entry_count), sizeof(entry_count));
            for (const auto &entry : *entries)
            {
                result.append(entry.serialize());
            }
        }
        else
        {
            uint32_t entry_count = 0;
            result.append(reinterpret_cast<const char *>(&entry_count), sizeof(entry_count));
        }

        result.append(reinterpret_cast<const char *>(&net_leader_commit), sizeof(net_leader_commit));
        return result;
    }

    static AppendEntriesData deserialize(const char *data, size_t &offset)
    {
        uint32_t net_prev_log_index, net_prev_log_term, net_leader_commit;

        std::memcpy(&net_prev_log_index, data + offset, sizeof(net_prev_log_index));
        offset += sizeof(net_prev_log_index);
        std::memcpy(&net_prev_log_term, data + offset, sizeof(net_prev_log_term));
        offset += sizeof(net_prev_log_term);

        uint32_t entry_count;
        std::memcpy(&entry_count, data + offset, sizeof(entry_count));
        offset += sizeof(entry_count);
        entry_count = ntohl(entry_count);

        std::optional<std::set<LogEntry>> entries_opt;
        if (entry_count > 0)
        {
            std::set<LogEntry> entry_set;
            for (uint32_t i = 0; i < entry_count; ++i)
            {
                entry_set.insert(LogEntry::deserialize(data, offset));
            }
            entries_opt = entry_set;
        }

        std::memcpy(&net_leader_commit, data + offset, sizeof(net_leader_commit));
        offset += sizeof(net_leader_commit);

        AppendEntriesData data_obj(ntohl(net_prev_log_index), ntohl(net_prev_log_term), ntohl(net_leader_commit));
        data_obj.entries = entries_opt;
        return data_obj;
    }
};

// 日志追加响应消息数据
struct AppendEntriesResponseData
{
    bool success; // 请求是否成功

    AppendEntriesResponseData(bool succ = false) : success(succ) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_success = static_cast<uint8_t>(success ? 1 : 0);
        result.append(reinterpret_cast<const char *>(&net_success), sizeof(net_success));
        return result;
    }

    static AppendEntriesResponseData deserialize(const char *data, size_t &offset)
    {
        uint8_t net_success;
        std::memcpy(&net_success, data + offset, sizeof(net_success));
        offset += sizeof(net_success);
        return AppendEntriesResponseData(net_success != 0);
    }
};

// 数据快照传输数据（支持分块传输，避免内存溢出）
struct DataSnapshotResponseData
{
    std::string serialize() const
    {
        return "";
    }

    static DataSnapshotResponseData deserialize(const char *data, size_t &offset)
    {
        return DataSnapshotResponseData();
    }
};
struct RaftMessage : public ProtocolBody
{
    RaftMessageType type = RaftMessageType::HEART_BEAT;                    // RPC 消息类型
    uint32_t term = 0;                                                     // 请求发送者任期
    std::optional<HeartBeatData> heartbeat_data;                           // 心跳数据
    std::optional<RequestVoteData> request_vote_data;                      // 请求投票数据
    std::optional<RequestVoteResponseData> request_vote_response_data;     // 请求投票响应数据
    std::optional<AppendEntriesData> append_entries_data;                  // 日志追加数据
    std::optional<AppendEntriesResponseData> append_entries_response_data; // 日志追加响应数据
    std::optional<NewConnection> new_connection;                           // 新连接数据
    std::optional<NewSlaver> new_slaver;                                   // 新从节点数据
    std::optional<DataSnapshotResponseData> data_snapshot_response_data;   // 数据快照响应数据

    RaftMessage() = default;

    static RaftMessage heart_beat(uint32_t term, uint32_t leader_commit)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::HEART_BEAT;
        msg.term = term;
        msg.heartbeat_data = HeartBeatData(leader_commit);
        return msg;
    }

    static RaftMessage request_vote(uint32_t term, uint32_t last_log_index)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::REQUEST_VOTE;
        msg.term = term;
        msg.request_vote_data = RequestVoteData(last_log_index);
        return msg;
    }

    static RaftMessage request_vote_response(uint32_t term, bool vote_granted)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::REQUEST_VOTE_RESPONSE;
        msg.term = term;
        msg.request_vote_response_data = RequestVoteResponseData(vote_granted);
        return msg;
    }

    static RaftMessage append_entries(uint32_t term, uint32_t prev_log_index,
                                      uint32_t prev_log_term, uint32_t leader_commit)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::APPEND_ENTRIES;
        msg.term = term;
        msg.append_entries_data = AppendEntriesData(prev_log_index, prev_log_term, leader_commit);
        return msg;
    }

    static RaftMessage append_entries_with_data(uint32_t term, uint32_t prev_log_index,
                                                uint32_t prev_log_term, const std::set<LogEntry> &entries,
                                                uint32_t leader_commit)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::APPEND_ENTRIES;
        msg.term = term;
        msg.append_entries_data = AppendEntriesData(prev_log_index, prev_log_term, entries, leader_commit);
        return msg;
    }

    static RaftMessage append_entries_response(uint32_t term, bool success)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::APPEND_ENTRIES_RESPONSE;
        msg.term = term;
        msg.append_entries_response_data = AppendEntriesResponseData(success);
        return msg;
    }

    static RaftMessage new_connection(bool is_reconnect, uint32_t last_index)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::NEW_CONNECTION;
        msg.new_connection = NewConnection(is_reconnect, last_index);
        return msg;
    }

    static RaftMessage new_slaver(std::string ip, uint16_t port)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::NEW_SLAVER;
        msg.new_slaver = NewSlaver(ip, port);
        return msg;
    }

    static RaftMessage new_master(uint32_t term)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::NEW_MASTER;
        msg.term = term;
        return msg;
    }

    static RaftMessage data_snapshot_response()
    {
        RaftMessage msg;
        msg.type = RaftMessageType::DATA_SNAPSHOT_RESPONSE;
        return msg;
    }

    std::string serialize() const
    {
        std::string result;
        uint32_t net_type = htonl(static_cast<uint32_t>(type));
        uint32_t net_term = htonl(term);

        result.append(reinterpret_cast<const char *>(&net_type), sizeof(net_type));
        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));

        switch (type)
        {
        case RaftMessageType::HEART_BEAT:
            if (heartbeat_data)
            {
                result.append(heartbeat_data->serialize());
            }
            break;

        case RaftMessageType::REQUEST_VOTE:
            if (request_vote_data)
            {
                result.append(request_vote_data->serialize());
            }
            break;

        case RaftMessageType::REQUEST_VOTE_RESPONSE:
            if (request_vote_response_data)
            {
                result.append(request_vote_response_data->serialize());
            }
            break;

        case RaftMessageType::APPEND_ENTRIES:
            if (append_entries_data)
            {
                result.append(append_entries_data->serialize());
            }
            break;

        case RaftMessageType::APPEND_ENTRIES_RESPONSE:
            if (append_entries_response_data)
            {
                result.append(append_entries_response_data->serialize());
            }
            break;

        case RaftMessageType::NEW_CONNECTION:
            if (new_connection)
            {
                result.append(new_connection->serialize());
            }
            break;

        case RaftMessageType::NEW_SLAVER:
            if (new_slaver)
            {
                result.append(new_slaver->serialize());
            }
            break;

        case RaftMessageType::DATA_SNAPSHOT_RESPONSE:
            if (data_snapshot_response_data)
            {
                result.append(data_snapshot_response_data->serialize());
            }
            break;

        case RaftMessageType::NEW_MASTER:
            break;

        default:
            break;
        }

        return result;
    }

    void deserialize(const char *data, size_t &offset)
    {
        uint32_t net_type, net_term;
        std::memcpy(&net_type, data + offset, sizeof(net_type));
        offset += sizeof(net_type);
        type = static_cast<RaftMessageType>(ntohl(net_type));
        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        term = ntohl(net_term);

        switch (type)
        {
        case RaftMessageType::HEART_BEAT:
            heartbeat_data = HeartBeatData::deserialize(data, offset);
            break;

        case RaftMessageType::REQUEST_VOTE:
            request_vote_data = RequestVoteData::deserialize(data, offset);
            break;

        case RaftMessageType::REQUEST_VOTE_RESPONSE:
            request_vote_response_data = RequestVoteResponseData::deserialize(data, offset);
            break;

        case RaftMessageType::APPEND_ENTRIES:
            append_entries_data = AppendEntriesData::deserialize(data, offset);
            break;

        case RaftMessageType::APPEND_ENTRIES_RESPONSE:
            append_entries_response_data = AppendEntriesResponseData::deserialize(data, offset);
            break;

        case RaftMessageType::NEW_CONNECTION:
            new_connection = NewConnection::deserialize(data, offset);
            break;

        case RaftMessageType::NEW_SLAVER:
            new_slaver = NewSlaver::deserialize(data, offset);
            break;

        case RaftMessageType::DATA_SNAPSHOT_RESPONSE:
            data_snapshot_response_data = DataSnapshotResponseData::deserialize(data, offset);
            break;

        case RaftMessageType::NEW_MASTER:
            break;

        default:
            break;
        }
    }
};
#endif