#ifndef RAFT_PROTOCOL_H_
#define RAFT_PROTOCOL_H_
#include <cstdint>
#include <string>
#include <cstring>
#include <set>
#include <optional>
#include <bit>
#include "common/socket/socket.h"
#include "common/socket/protocol/protocol.h"
#include "common/serialization/serializer.h"
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
// 日志缓冲区配置
#define RAFT_LOG_BUFFER_SIZE 8192 // 环形缓冲区大小（已弃用，使用RaftLogArray代替）

using Serializer = EyaKV::Serializer;

//  RPC 消息类型
enum RaftMessageType
{
    HEART_BEAT = 0,                 // 心跳消息
    REQUEST_VOTE = 1,               // 请求投票
    REQUEST_VOTE_RESPONSE = 2,      // 请求投票响应
    APPEND_ENTRIES = 3,             // 日志追加
    APPEND_ENTRIES_RESPONSE = 4,    // 日志追加响应
    NEW_NODE = 5,                   // 新节点加入广播
    NEW_MASTER = 7,                 // 新主节点选举成功消息
    QUERY_LEADER = 9,               // 探查leader
    QUERY_LEADER_RESPONSE = 10,     // 探查leader响应
    JOIN_CLUSTER = 11,              // 新节点加入集群
    JOIN_CLUSTER_RESPONSE = 12,     // 新节点加入响应
    CLUSTER_CONFIG = 13,            // 集群配置变更
    CLUSTER_CONFIG_RESPONSE = 14,   // 集群配置变更响应
    SNAPSHOT_INSTALL = 15,          // 快照安装
    SNAPSHOT_INSTALL_RESPONSE = 16, // 快照安装响应
    LOG_SYNC_REQUEST = 17,          // 日志同步请求
    LOG_SYNC_RESPONSE = 18,         // 日志同步响应
    LEAVE_NODE = 19                 // 节点移除
};
// 日志条目结构体（根据设计文档更新）
struct LogEntry
{
    uint32_t term;         // 日志所属任期
    uint32_t index;        // 日志索引（全局唯一）
    uint32_t command_type; // 命令类型 (预留字段，用于区分普通命令、配置变更等)
    std::string cmd;       // 客户端命令
    uint64_t timestamp;    // 时间戳 (用于调试和超时检测)

    LogEntry() : term(0), index(0), command_type(0), timestamp(0) {}
    LogEntry(uint32_t t, uint32_t i, const std::string &c, uint64_t ts = 0, uint32_t cmd_type = 0)
        : term(t), index(i), command_type(cmd_type), cmd(c), timestamp(ts) {}

    // 序列化
    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint32_t net_index = htonl(index);
        uint32_t net_command_type = htonl(command_type);
        uint32_t net_timestamp = htonl(static_cast<uint32_t>(timestamp >> 32));
        uint32_t net_timestamp_low = htonl(static_cast<uint32_t>(timestamp & 0xFFFFFFFF));
        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        result.append(reinterpret_cast<const char *>(&net_index), sizeof(net_index));
        result.append(reinterpret_cast<const char *>(&net_command_type), sizeof(net_command_type));
        result.append(reinterpret_cast<const char *>(&net_timestamp), sizeof(net_timestamp));
        result.append(reinterpret_cast<const char *>(&net_timestamp_low), sizeof(net_timestamp_low));
        result.append(Serializer::serialize(cmd));
        return result;
    }

    // 反序列化
    static LogEntry deserialize(const char *data, size_t &offset)
    {
        LogEntry entry;
        uint32_t net_term, net_index, net_command_type, net_timestamp, net_timestamp_low;
        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        entry.term = ntohl(net_term);
        std::memcpy(&net_index, data + offset, sizeof(net_index));
        offset += sizeof(net_index);
        entry.index = ntohl(net_index);
        std::memcpy(&net_command_type, data + offset, sizeof(net_command_type));
        offset += sizeof(net_command_type);
        entry.command_type = ntohl(net_command_type);
        std::memcpy(&net_timestamp, data + offset, sizeof(net_timestamp));
        offset += sizeof(net_timestamp);
        std::memcpy(&net_timestamp_low, data + offset, sizeof(net_timestamp_low));
        offset += sizeof(net_timestamp_low);
        entry.timestamp = (static_cast<uint64_t>(ntohl(net_timestamp)) << 32) | ntohl(net_timestamp_low);
        // 反序列化cmd字符串
        entry.cmd = Serializer::deserializeString(data, offset);
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
    bool success;       // 请求是否成功
    uint32_t log_index; // 最终的日志索引
    AppendEntriesResponseData(bool succ = false, uint32_t idx = 0) : success(succ), log_index(idx) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_success = static_cast<uint8_t>(success ? 1 : 0);
        uint32_t net_log_index = htonl(log_index);
        result.append(reinterpret_cast<const char *>(&net_success), sizeof(net_success));
        result.append(reinterpret_cast<const char *>(&net_log_index), sizeof(net_log_index));
        return result;
    }

    static AppendEntriesResponseData deserialize(const char *data, size_t &offset)
    {
        uint8_t net_success;
        uint32_t net_log_index;
        std::memcpy(&net_success, data + offset, sizeof(net_success));
        offset += sizeof(net_success);
        std::memcpy(&net_log_index, data + offset, sizeof(net_log_index));
        offset += sizeof(net_log_index);
        return AppendEntriesResponseData(net_success != 0, ntohl(net_log_index));
    }
};

// 新节点加入广播
struct NewNodeJoinData
{
    Address node_address;

    NewNodeJoinData(const Address &addr)
        : node_address(addr) {}

    std::string serialize() const
    {
        return node_address.serialize();
    }

    static NewNodeJoinData deserialize(const char *data, size_t &offset)
    {
        Address addr = Address::deserialize(data, offset);
        return NewNodeJoinData(addr);
    }
};
// 集群元数据结构
struct ClusterMetadata
{
    std::unordered_set<Address> cluster_nodes_; // 集群所有节点地址列表
    Address current_leader_;                    // 当前leader地址 (可能为空)
    uint32_t cluster_version_;                  // 集群配置版本号 (用于联合共识)

    ClusterMetadata() : cluster_nodes_(), current_leader_(), cluster_version_(0) {}

    std::string serialize() const
    {
        std::string result;
        uint32_t net_version = htonl(cluster_version_);
        result.append(reinterpret_cast<const char *>(&net_version), sizeof(net_version));

        result.append(current_leader_.serialize());

        uint32_t node_count = htonl(static_cast<uint32_t>(cluster_nodes_.size()));
        result.append(reinterpret_cast<const char *>(&node_count), sizeof(node_count));
        for (const auto &node : cluster_nodes_)
        {
            result.append(node.serialize());
        }
        return result;
    }

    static ClusterMetadata deserialize(const char *data, size_t &offset)
    {
        ClusterMetadata metadata;
        uint32_t net_version;
        std::memcpy(&net_version, data + offset, sizeof(net_version));
        offset += sizeof(net_version);
        metadata.cluster_version_ = ntohl(net_version);

        metadata.current_leader_ = Address::deserialize(data, offset);

        uint32_t node_count;
        std::memcpy(&node_count, data + offset, sizeof(node_count));
        offset += sizeof(node_count);
        node_count = ntohl(node_count);
        for (uint32_t i = 0; i < node_count; ++i)
        {
            metadata.cluster_nodes_.insert(Address::deserialize(data, offset));
        }
        return metadata;
    }
};

// 探查leader响应消息
struct QueryLeaderResponseData
{
    Address leader_address;

    QueryLeaderResponseData() : leader_address() {}
    QueryLeaderResponseData(const Address &addr)
        : leader_address(addr) {}
    std::string serialize() const
    {
        return leader_address.serialize();
    }
    static QueryLeaderResponseData deserialize(const char *data, size_t &offset)
    {
        Address addr = Address::deserialize(data, offset);
        return QueryLeaderResponseData(addr);
    }
};
struct LeaveNodeData
{
    Address node_address;

    LeaveNodeData() : node_address() {}
    LeaveNodeData(const Address &addr) : node_address(addr) {}

    std::string serialize() const
    {
        return node_address.serialize();
    }

    static LeaveNodeData deserialize(const char *data, size_t &offset)
    {
        Address addr = Address::deserialize(data, offset);
        return LeaveNodeData(addr);
    }
};

// 新节点加入集群消息
struct JoinClusterData
{
    bool is_reconnect;   // 是否是重连
    uint32_t last_index; // 日志的最后一个索引
    JoinClusterData() : is_reconnect(false), last_index(0) {}
    JoinClusterData(bool reconnect, uint32_t last_idx)
        : is_reconnect(reconnect), last_index(last_idx) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_is_reconnect = static_cast<uint8_t>(is_reconnect ? 1 : 0);

        result.append(reinterpret_cast<const char *>(&net_is_reconnect), sizeof(net_is_reconnect));

        // 序列化密码和日志索引
        uint32_t net_last_index = htonl(last_index);
        result.append(reinterpret_cast<const char *>(&net_last_index), sizeof(net_last_index));

        return result;
    }
    static JoinClusterData deserialize(const char *data, size_t &offset)
    {
        JoinClusterData join_data;
        uint8_t net_is_reconnect;
        std::memcpy(&net_is_reconnect, data + offset, sizeof(net_is_reconnect));
        offset += sizeof(net_is_reconnect);
        join_data.is_reconnect = (net_is_reconnect != 0);

        uint32_t net_last_index;
        std::memcpy(&net_last_index, data + offset, sizeof(net_last_index));
        offset += sizeof(net_last_index);
        join_data.last_index = ntohl(net_last_index);

        return join_data;
    }
};

// 新节点加入集群响应
struct JoinClusterResponseData
{
    bool success;
    std::string error_message;
    ClusterMetadata cluster_metadata;
    uint32_t sync_from_index;

    JoinClusterResponseData() : success(false), sync_from_index(0) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_success = static_cast<uint8_t>(success ? 1 : 0);
        result.append(reinterpret_cast<const char *>(&net_success), sizeof(net_success));

        if (success)
        {
            result.append(cluster_metadata.serialize());

            uint32_t net_sync_index = htonl(sync_from_index);
            result.append(reinterpret_cast<const char *>(&net_sync_index), sizeof(net_sync_index));
        }
        else
        {
            uint32_t err_len = htonl(static_cast<uint32_t>(error_message.size()));
            result.append(reinterpret_cast<const char *>(&err_len), sizeof(err_len));
            result.append(error_message);
        }
        return result;
    }

    static JoinClusterResponseData deserialize(const char *data, size_t &offset)
    {
        JoinClusterResponseData response;
        uint8_t net_success;
        std::memcpy(&net_success, data + offset, sizeof(net_success));
        offset += sizeof(net_success);
        response.success = (net_success != 0);

        if (response.success)
        {
            response.cluster_metadata = ClusterMetadata::deserialize(data, offset);

            uint32_t net_sync_index;
            std::memcpy(&net_sync_index, data + offset, sizeof(net_sync_index));
            offset += sizeof(net_sync_index);
            response.sync_from_index = ntohl(net_sync_index);
        }
        else
        {
            uint32_t err_len;
            std::memcpy(&err_len, data + offset, sizeof(err_len));
            offset += sizeof(err_len);
            err_len = ntohl(err_len);
            response.error_message.assign(data + offset, err_len);
            offset += err_len;
        }
        return response;
    }
};

// 快照安装消息
struct SnapshotInstallData
{
    uint32_t term;
    std::string leader_id;
    uint32_t last_included_index;
    uint32_t last_included_term;
    std::string snapshot_data;
    uint32_t offset;
    bool done;

    SnapshotInstallData() : term(0), last_included_index(0), last_included_term(0), offset(0), done(false) {}

    std::string serialize() const
    {
        std::string result;
        uint32_t net_term = htonl(term);
        uint32_t net_last_index = htonl(last_included_index);
        uint32_t net_last_term = htonl(last_included_term);
        uint32_t net_offset = htonl(offset);
        uint8_t net_done = static_cast<uint8_t>(done ? 1 : 0);

        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));

        uint32_t leader_len = htonl(static_cast<uint32_t>(leader_id.size()));
        result.append(reinterpret_cast<const char *>(&leader_len), sizeof(leader_len));
        result.append(leader_id);

        result.append(reinterpret_cast<const char *>(&net_last_index), sizeof(net_last_index));
        result.append(reinterpret_cast<const char *>(&net_last_term), sizeof(net_last_term));

        uint32_t data_len = htonl(static_cast<uint32_t>(snapshot_data.size()));
        result.append(reinterpret_cast<const char *>(&data_len), sizeof(data_len));
        result.append(snapshot_data);

        result.append(reinterpret_cast<const char *>(&net_offset), sizeof(net_offset));
        result.append(reinterpret_cast<const char *>(&net_done), sizeof(net_done));

        return result;
    }

    static SnapshotInstallData deserialize(const char *data, size_t &offset)
    {
        SnapshotInstallData snapshot;
        uint32_t net_term, net_last_index, net_last_term, net_offset;
        uint8_t net_done;

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        snapshot.term = ntohl(net_term);

        uint32_t leader_len;
        std::memcpy(&leader_len, data + offset, sizeof(leader_len));
        offset += sizeof(leader_len);
        leader_len = ntohl(leader_len);
        snapshot.leader_id.assign(data + offset, leader_len);
        offset += leader_len;

        std::memcpy(&net_last_index, data + offset, sizeof(net_last_index));
        offset += sizeof(net_last_index);
        snapshot.last_included_index = ntohl(net_last_index);

        std::memcpy(&net_last_term, data + offset, sizeof(net_last_term));
        offset += sizeof(net_last_term);
        snapshot.last_included_term = ntohl(net_last_term);

        uint32_t data_len;
        std::memcpy(&data_len, data + offset, sizeof(data_len));
        offset += sizeof(data_len);
        data_len = ntohl(data_len);
        snapshot.snapshot_data.assign(data + offset, data_len);
        offset += data_len;

        std::memcpy(&net_offset, data + offset, sizeof(net_offset));
        offset += sizeof(net_offset);
        snapshot.offset = ntohl(net_offset);

        std::memcpy(&net_done, data + offset, sizeof(net_done));
        offset += sizeof(net_done);
        snapshot.done = (net_done != 0);

        return snapshot;
    }
};

// 快照安装响应
struct SnapshotInstallResponseData
{
    bool success;
    uint32_t term;

    SnapshotInstallResponseData() : success(false), term(0) {}
    SnapshotInstallResponseData(bool succ, uint32_t t) : success(succ), term(t) {}

    std::string serialize() const
    {
        std::string result;
        uint8_t net_success = static_cast<uint8_t>(success ? 1 : 0);
        uint32_t net_term = htonl(term);
        result.append(reinterpret_cast<const char *>(&net_success), sizeof(net_success));
        result.append(reinterpret_cast<const char *>(&net_term), sizeof(net_term));
        return result;
    }

    static SnapshotInstallResponseData deserialize(const char *data, size_t &offset)
    {
        SnapshotInstallResponseData response;
        uint8_t net_success;
        uint32_t net_term;

        std::memcpy(&net_success, data + offset, sizeof(net_success));
        offset += sizeof(net_success);
        response.success = (net_success != 0);

        std::memcpy(&net_term, data + offset, sizeof(net_term));
        offset += sizeof(net_term);
        response.term = ntohl(net_term);
        return response;
    }
};

struct RaftMessage : public ProtocolBody
{
    RaftMessageType type = RaftMessageType::HEART_BEAT;                        // RPC 消息类型
    uint32_t term = 0;                                                         // 请求发送者任期
    std::optional<HeartBeatData> heartbeat_data;                               // 心跳数据
    std::optional<RequestVoteData> request_vote_data;                          // 请求投票数据
    std::optional<RequestVoteResponseData> request_vote_response_data;         // 请求投票响应数据
    std::optional<AppendEntriesData> append_entries_data;                      // 日志追加数据
    std::optional<AppendEntriesResponseData> append_entries_response_data;     // 日志追加响应数据
    std::optional<QueryLeaderResponseData> query_leader_response_data;         // 探查leader响应数据
    std::optional<JoinClusterData> join_cluster_data;                          // 新节点加入集群数据
    std::optional<JoinClusterResponseData> join_cluster_response_data;         // 新节点加入集群响应数据
    std::optional<SnapshotInstallData> snapshot_install_data;                  // 快照安装数据
    std::optional<SnapshotInstallResponseData> snapshot_install_response_data; // 快照安装响应数据
    std::optional<NewNodeJoinData> new_node_join_data;                         // 新连接数据
    std::optional<LeaveNodeData> leave_node_data;                              // 节点移除数据
    RaftMessage() = default;

    static RaftMessage leave_node(const Address &addr)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::LEAVE_NODE;
        msg.leave_node_data = LeaveNodeData(addr);
        return msg;
    }

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

    static RaftMessage append_entries_response(uint32_t term, bool success, uint32_t last_log_index)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::APPEND_ENTRIES_RESPONSE;
        msg.term = term;
        msg.append_entries_response_data = AppendEntriesResponseData(success, last_log_index);
        return msg;
    }

    static RaftMessage new_node_join_broadcast(Address addr)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::NEW_NODE;
        msg.new_node_join_data = NewNodeJoinData(addr);
        return msg;
    }

    static RaftMessage new_master(uint32_t term)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::NEW_MASTER;
        msg.term = term;
        return msg;
    }

    // 新增消息工厂函数
    static RaftMessage query_leader()
    {
        RaftMessage msg;
        msg.type = RaftMessageType::QUERY_LEADER;
        return msg;
    }

    static RaftMessage query_leader_response(const Address &addr)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::QUERY_LEADER_RESPONSE;
        msg.query_leader_response_data = QueryLeaderResponseData(addr);
        return msg;
    }

    static RaftMessage join_cluster(bool is_reconnect, uint32_t last_index)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::JOIN_CLUSTER;
        msg.join_cluster_data = JoinClusterData(is_reconnect, last_index);
        return msg;
    }

    static RaftMessage join_cluster_response(bool success, const std::string &error,
                                             const ClusterMetadata &meta, uint32_t sync_index)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::JOIN_CLUSTER_RESPONSE;
        msg.join_cluster_response_data = JoinClusterResponseData();
        msg.join_cluster_response_data->success = success;
        msg.join_cluster_response_data->error_message = error;
        msg.join_cluster_response_data->cluster_metadata = meta;
        msg.join_cluster_response_data->sync_from_index = sync_index;
        return msg;
    }

    static RaftMessage snapshot_install(uint32_t term, const std::string &leader_id,
                                        uint32_t last_index, uint32_t last_term,
                                        const std::string &data, uint32_t offset, bool done)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::SNAPSHOT_INSTALL;
        msg.snapshot_install_data = SnapshotInstallData();
        msg.snapshot_install_data->term = term;
        msg.snapshot_install_data->leader_id = leader_id;
        msg.snapshot_install_data->last_included_index = last_index;
        msg.snapshot_install_data->last_included_term = last_term;
        msg.snapshot_install_data->snapshot_data = data;
        msg.snapshot_install_data->offset = offset;
        msg.snapshot_install_data->done = done;
        return msg;
    }

    static RaftMessage snapshot_install_response(bool success, uint32_t term)
    {
        RaftMessage msg;
        msg.type = RaftMessageType::SNAPSHOT_INSTALL_RESPONSE;
        msg.snapshot_install_response_data = SnapshotInstallResponseData(success, term);
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
            if (heartbeat_data.has_value())
            {
                result.append(heartbeat_data->serialize());
            }
            break;

        case RaftMessageType::REQUEST_VOTE:
            if (request_vote_data.has_value())
            {
                result.append(request_vote_data->serialize());
            }
            break;

        case RaftMessageType::REQUEST_VOTE_RESPONSE:
            if (request_vote_response_data.has_value())
            {
                result.append(request_vote_response_data->serialize());
            }
            break;

        case RaftMessageType::APPEND_ENTRIES:
            if (append_entries_data.has_value())
            {
                result.append(append_entries_data->serialize());
            }
            break;

        case RaftMessageType::APPEND_ENTRIES_RESPONSE:
            if (append_entries_response_data.has_value())
            {
                result.append(append_entries_response_data->serialize());
            }
            break;

        case RaftMessageType::NEW_NODE:
            if (new_node_join_data.has_value())
            {
                result.append(new_node_join_data->serialize());
            }
            break;

        case RaftMessageType::QUERY_LEADER_RESPONSE:
            if (query_leader_response_data.has_value())
            {
                result.append(query_leader_response_data->serialize());
            }
            break;

        case RaftMessageType::JOIN_CLUSTER:
            if (join_cluster_data.has_value())
            {
                result.append(join_cluster_data->serialize());
            }
            break;

        case RaftMessageType::JOIN_CLUSTER_RESPONSE:
            if (join_cluster_response_data.has_value())
            {
                result.append(join_cluster_response_data->serialize());
            }
            break;

        case RaftMessageType::SNAPSHOT_INSTALL:
            if (snapshot_install_data.has_value())
            {
                result.append(snapshot_install_data->serialize());
            }
            break;

        case RaftMessageType::SNAPSHOT_INSTALL_RESPONSE:
            if (snapshot_install_response_data.has_value())
            {
                result.append(snapshot_install_response_data->serialize());
            }
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

        case RaftMessageType::NEW_NODE:
            new_node_join_data = NewNodeJoinData::deserialize(data, offset);
            break;

        case RaftMessageType::QUERY_LEADER_RESPONSE:
            query_leader_response_data = QueryLeaderResponseData::deserialize(data, offset);
            break;

        case RaftMessageType::JOIN_CLUSTER:
            join_cluster_data = JoinClusterData::deserialize(data, offset);
            break;

        case RaftMessageType::JOIN_CLUSTER_RESPONSE:
            join_cluster_response_data = JoinClusterResponseData::deserialize(data, offset);
            break;

        case RaftMessageType::SNAPSHOT_INSTALL:
            snapshot_install_data = SnapshotInstallData::deserialize(data, offset);
            break;

        case RaftMessageType::SNAPSHOT_INSTALL_RESPONSE:
            snapshot_install_response_data = SnapshotInstallResponseData::deserialize(data, offset);
            break;

        default:
            break;
        }
    }
};
#endif