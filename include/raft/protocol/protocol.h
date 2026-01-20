#ifndef RAFT_PROTOCOL_H_
#define RAFT_PROTOCOL_H_
#include <cstdint>
#include <string>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
//  日志缓冲区配置
#define RAFT_LOG_BUFFER_SIZE 8192 // 环形缓冲区大小

//  RPC 消息类型
enum RaftMessageType
{
    REQUEST_VOTE = 1,             // 请求投票
    REQUEST_VOTE_RESPONSE = 2,    // 请求投票响应
    APPEND_ENTRIES = 3,           // 日志追加
    APPEND_ENTRIES_RESPONSE = 4,  // 日志追加响应
    INSTALL_SNAPSHOT = 5,         // 安装快照
    INSTALL_SNAPSHOT_RESPONSE = 6 // 安装快照响应
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
};

// 请求投票请求
struct RequestVoteRequest
{
    uint32_t term;           // 候选人的任期
    int candidate_id;        // 候选人的ID
    uint32_t last_log_index; // 候选人最后一条日志的索引
    uint32_t last_log_term;  // 候选人最后一条日志的任期

    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint32_t net_last_index = htonl(last_log_index);
        uint32_t net_last_term = htonl(last_log_term);
        int32_t net_id = htonl(candidate_id);

        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&net_id), sizeof(net_id));
        result.append(reinterpret_cast<const char *>(&net_last_index), sizeof(net_last_index));
        result.append(reinterpret_cast<const char *>(&net_last_term), sizeof(net_last_term));
        return result;
    }

    static RequestVoteRequest deserialize(const char *data, size_t &offset)
    {
        RequestVoteRequest req;
        uint32_t net_term, net_last_index, net_last_term;
        int32_t net_id;

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        req.term = ntohl(net_term);
        std::memcpy(&net_id, data + offset, sizeof(net_id));
        offset += sizeof(net_id);
        req.candidate_id = ntohl(net_id);
        std::memcpy(&net_last_index, data + offset, sizeof(net_last_index));
        offset += sizeof(net_last_index);
        req.last_log_index = ntohl(net_last_index);
        std::memcpy(&net_last_term, data + offset, sizeof(net_last_term));
        offset += sizeof(net_last_term);
        req.last_log_term = ntohl(net_last_term);
        return req;
    }
};

// 请求投票响应
struct RequestVoteResponse
{
    uint32_t term;     // 当前任期
    bool vote_granted; // 是否投票

    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint8_t granted = vote_granted ? 1 : 0;

        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&granted), sizeof(granted));
        return result;
    }

    static RequestVoteResponse deserialize(const char *data, size_t &offset)
    {
        RequestVoteResponse resp;
        uint32_t net_term;
        uint8_t granted;

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        resp.term = ntohl(net_term);
        std::memcpy(&granted, data + offset, sizeof(granted));
        offset += sizeof(granted);
        resp.vote_granted = (granted == 1);
        return resp;
    }
};

// 追加条目请求
struct AppendEntriesRequest
{
    uint32_t term;                 // Leader的任期
    int leader_id;                 // Leader的ID
    uint32_t prev_log_index;       // 前一条日志的索引
    uint32_t prev_log_term;        // 前一条日志的任期
    std::vector<LogEntry> entries; // 要追加的日志条目
    uint32_t leader_commit;        // Leader的commitIndex

    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        int32_t net_id = htonl(leader_id);
        uint32_t net_prev_index = htonl(prev_log_index);
        uint32_t net_prev_term = htonl(prev_log_term);
        uint32_t net_commit = htonl(leader_commit);
        uint32_t entry_count = htonl(static_cast<uint32_t>(entries.size()));

        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&net_id), sizeof(net_id));
        result.append(reinterpret_cast<const char *>(&net_prev_index), sizeof(net_prev_index));
        result.append(reinterpret_cast<const char *>(&net_prev_term), sizeof(net_prev_term));
        result.append(reinterpret_cast<const char *>(&net_commit), sizeof(net_commit));
        result.append(reinterpret_cast<const char *>(&entry_count), sizeof(entry_count));

        for (const auto &entry : entries)
        {
            result.append(entry.serialize());
        }
        return result;
    }

    static AppendEntriesRequest deserialize(const char *data, size_t &offset)
    {
        AppendEntriesRequest req;
        uint32_t net_term, net_prev_index, net_prev_term, net_commit, entry_count;
        int32_t net_id;

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        req.term = ntohl(net_term);
        std::memcpy(&net_id, data + offset, sizeof(net_id));
        offset += sizeof(net_id);
        req.leader_id = ntohl(net_id);
        std::memcpy(&net_prev_index, data + offset, sizeof(net_prev_index));
        offset += sizeof(net_prev_index);
        req.prev_log_index = ntohl(net_prev_index);
        std::memcpy(&net_prev_term, data + offset, sizeof(net_prev_term));
        offset += sizeof(net_prev_term);
        req.prev_log_term = ntohl(net_prev_term);
        std::memcpy(&net_commit, data + offset, sizeof(net_commit));
        offset += sizeof(net_commit);
        req.leader_commit = ntohl(net_commit);
        std::memcpy(&entry_count, data + offset, sizeof(entry_count));
        offset += sizeof(entry_count);
        entry_count = ntohl(entry_count);

        for (uint32_t i = 0; i < entry_count; ++i)
        {
            req.entries.push_back(LogEntry::deserialize(data, offset));
        }
        return req;
    }
};

// 追加条目响应
struct AppendEntriesResponse
{
    uint32_t term; // 当前任期
    bool success;  // 是否成功

    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint8_t succ = success ? 1 : 0;

        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&succ), sizeof(succ));
        return result;
    }

    static AppendEntriesResponse deserialize(const char *data, size_t &offset)
    {
        AppendEntriesResponse resp;
        uint32_t net_term;
        uint8_t succ;

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        resp.term = ntohl(net_term);
        std::memcpy(&succ, data + offset, sizeof(succ));
        offset += sizeof(succ);
        resp.success = (succ == 1);
        return resp;
    }
};

#endif