#ifndef EYAKV_RAFT_RAFT_H_
#define EYAKV_RAFT_RAFT_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <cstring>
#include <shared_mutex>
#include "common/base/export.h"
#include "raft/protocol/protocol.h"

//  Raft角色枚举
enum RaftRole
{
    Follower,
    Candidate,
    Leader
};

//  环形日志缓冲区（Redis偏移环形式）
class EYAKV_RAFT_API RaftLogRingBuffer
{
private:
    std::vector<LogEntry> buffer_; // 固定大小的缓冲区
    uint32_t tail_;                // 写指针（下一个写入位置）
    uint32_t base_index_;          // 基准索引（环形缓冲区起始对应的逻辑索引）
    mutable std::shared_mutex mutex_;

    // 计算实际缓冲区索引
    inline uint32_t to_buffer_index(uint32_t logical_index) const
    {
        return logical_index % buffer_.size();
    }

public:
    explicit RaftLogRingBuffer(uint32_t size = RAFT_LOG_BUFFER_SIZE)
        : buffer_(size), tail_(0), base_index_(0)
    {
    }

    RaftLogRingBuffer(uint32_t base_index, uint32_t tail, std::vector<LogEntry> &buffer)
    {
        base_index_ = base_index;
        tail_ = tail;
        buffer_ = std::move(buffer);
    }

    ~RaftLogRingBuffer() = default;
    // 追加日志条目
    bool append(const LogEntry &entry)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        uint32_t buffer_idx = to_buffer_index(tail_);
        buffer_[buffer_idx] = entry;
        tail_++;

        // 如果缓冲区满了，移动head指针
        if (tail_ - base_index_ > buffer_.size())
        {
            base_index_++;
        }
        return true;
    }

    // 获取指定索引的日志条目
    bool get(uint32_t index, LogEntry &entry) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (index < base_index_ || index >= tail_)
        {
            return false; // 索引超出范围
        }

        uint32_t buffer_idx = to_buffer_index(index);
        entry = buffer_[buffer_idx];
        return true;
    }

    // 截断日志（从某索引开始删除）
    bool truncate_from(uint32_t index)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (index < base_index_ || index > tail_)
        {
            return false;
        }

        tail_ = index;
        return true;
    }

    // 获取最后一条日志
    bool get_last(LogEntry &entry) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (tail_ == 0)
        {
            return false;
        }

        uint32_t buffer_idx = to_buffer_index(tail_ - 1);
        entry = buffer_[buffer_idx];
        return true;
    }

    // 获取最后一条日志的索引和任期
    bool get_last_info(uint32_t &index, uint32_t &term) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (tail_ == 0)
        {
            index = 0;
            term = 0;
            return false;
        }

        index = tail_ - 1;
        uint32_t buffer_idx = to_buffer_index(tail_ - 1);
        term = buffer_[buffer_idx].term;
        return true;
    }

    // 获取指定索引的日志任期
    uint32_t get_term(uint32_t index) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (index < base_index_ || index >= tail_)
        {
            return 0;
        }

        uint32_t buffer_idx = to_buffer_index(index);
        return buffer_[buffer_idx].term;
    }

    // 获取日志数量
    uint32_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return tail_ - base_index_;
    }

    // 获取最后的日志索引
    uint32_t get_last_index() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return tail_ > 0 ? tail_ - 1 : 0;
    }
    // 获取基准索引（最老的日志索引）
    uint32_t get_base_index() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return base_index_;
    }

    // 获取从指定索引开始的所有日志条目
    std::vector<LogEntry> get_entries_from(uint32_t start_index) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<LogEntry> entries;

        if (start_index < base_index_ || start_index > tail_)
        {
            return entries;
        }

        for (uint32_t i = start_index; i < tail_; ++i)
        {
            uint32_t buffer_idx = to_buffer_index(i - base_index_);
            entries.push_back(buffer_[buffer_idx]);
        }

        return entries;
    }

    // 序列化
    std::string serialize() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::string data;
        uint32_t base_index = htonl(base_index_);
        uint32_t tail = htonl(tail_);
        data.append(reinterpret_cast<const char *>(&base_index), sizeof(base_index));
        data.append(reinterpret_cast<const char *>(&tail), sizeof(tail));
        uint32_t size = htonl(tail_ - base_index_);
        data.append(reinterpret_cast<const char *>(&size), sizeof(size));
        for (uint32_t i = base_index_; i < tail_; ++i)
        {
            uint32_t buffer_idx = to_buffer_index(i);
            data.append(buffer_[buffer_idx].serialize());
        }
        return data;
    }
    // 反序列化
    static RaftLogRingBuffer *deserialize(const char *data, size_t &offset)
    {
        uint32_t base_index, tail, size;
        std::memcpy(&base_index, data + offset, sizeof(base_index));
        offset += sizeof(base_index);
        std::memcpy(&tail, data + offset, sizeof(tail));
        offset += sizeof(tail);
        std::memcpy(&size, data + offset, sizeof(size));
        offset += sizeof(size);
        base_index = ntohl(base_index);
        tail = ntohl(tail);
        size = ntohl(size);
        std::vector<LogEntry> buffer(size);
        for (uint32_t i = base_index; i < tail; ++i)
        {
            LogEntry entry = LogEntry::deserialize(data, offset);
            buffer[i % size] = entry;
        }
        return new RaftLogRingBuffer(base_index, tail, buffer);
    }
};

struct RaftNodeAddress
{
    std::string host;
    int port;
    RaftNodeAddress(const std::string &host, int port) : host(host), port(port) {}

    RaftNodeAddress() : host(""), port(0) {}

    bool operator==(const RaftNodeAddress &other) const
    {
        return host == other.host && port == other.port;
    }

    bool is_null() const
    {
        return host.empty() && port == 0;
    }

    std::string to_string() const
    {
        return host + ":" + std::to_string(port);
    }

    std::string serialize() const
    {
        std::string data;
        uint32_t host_len = host.size();
        host_len = htonl(host_len);
        data.append(reinterpret_cast<const char *>(&host_len), sizeof(host_len));
        data.append(host);
        uint32_t port_net = htonl(port);
        data.append(reinterpret_cast<const char *>(&port_net), sizeof(port_net));
        return data;
    }

    static RaftNodeAddress deserialize(const char *data, size_t &offset)
    {
        uint32_t host_len;
        std::memcpy(&host_len, data + offset, sizeof(host_len));
        offset += sizeof(host_len);
        host_len = ntohl(host_len);
        std::string host(data + offset, host_len);
        offset += host_len;
        uint32_t port_net;
        std::memcpy(&port_net, data + offset, sizeof(port_net));
        offset += sizeof(port_net);
        port_net = ntohl(port_net);
        return RaftNodeAddress(host, port_net);
    }
};

namespace std
{
    template <>
    struct hash<RaftNodeAddress>
    {
        std::size_t operator()(const RaftNodeAddress &addr) const
        {
            std::string data = addr.to_string();
            return std::hash<std::string>()(data);
        }
    };
}

//  Raft节点类

//  Raft节点类
class EYAKV_RAFT_API RaftNode
{
private:
    //  持久化状态
    std::atomic<uint32_t> current_term_;     // 当前任期
    std::atomic<RaftNodeAddress> voted_for_; // 本任期投票给的节点（-1表示未投票）
    RaftLogRingBuffer log_buffer_;           // 环形日志缓冲区
    std::atomic<uint32_t> commit_index_;     // 已提交的最高日志索引
    std::atomic<uint32_t> last_applied_;     // 已应用到状态机的最高日志索引

    //  易失性状态
    std::atomic<RaftRole> role_; // 角色

    //  易失性状态（仅 Leader）
    std::unordered_map<RaftNodeAddress, uint32_t> next_index_;  // 每个Follower下一个要发送的日志索引
    std::unordered_map<RaftNodeAddress, uint32_t> match_index_; // 每个Follower已匹配的日志索引

    //  集群配置
    std::vector<RaftNodeAddress> cluster_nodes_; // 集群所有节点ID
    std::atomic<RaftNodeAddress> leader_id_;     // 当前感知的Leader ID（-1表示无）

    //  超时配置（毫秒）
    int election_timeout_min_ = 150; // 选举超时最小值
    int election_timeout_max_ = 300; // 选举超时最大值
    int heartbeat_interval_ = 30;    // 心跳间隔

    //  随机数生成（用于选举超时）
    std::mt19937 rng_;
    int election_timeout_;
    std::chrono::steady_clock::time_point last_heartbeat_time_;

    //  互斥锁（保证多线程安全）
    mutable std::mutex mutex_;

    //  线程控制
    std::atomic<bool> running_;
    std::thread election_thread_;
    std::thread heartbeat_thread_;

    // 生成随机选举超时时间
    int generate_election_timeout()
    {
        std::uniform_int_distribution<int> dist(election_timeout_min_, election_timeout_max_);
        return dist(rng_);
    }

    // 重置选举超时（更新最后心跳时间+重新生成超时值）
    void reset_election_timeout()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_heartbeat_time_ = std::chrono::steady_clock::now();
        election_timeout_ = generate_election_timeout();
    }

    // 检查是否触发选举超时
    bool is_election_timeout()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat_time_).count();
        return elapsed > election_timeout_;
    }

    // 选举线程主循环
    void election_loop();

    // 心跳线程主循环
    void heartbeat_loop();

    // 转换为Follower
    void become_follower(uint32_t term);

    // 转换为Candidate
    void become_candidate();

    // 转换为Leader
    void become_leader();

    // 发送RequestVote请求
    void send_request_vote();

    // 发送AppendEntries（心跳或日志复制）
    void send_append_entries(int follower_id, bool is_heartbeat = false);

    // 应用已提交的日志到状态机
    void apply_committed_entries();

public:
    RaftNode();
    ~RaftNode();

    // 启动节点
    void start();

    // 停止节点
    void stop();

    // 处理RequestVote请求
    RequestVoteResponse handle_request_vote(const RequestVoteRequest &req);

    // 处理AppendEntries请求
    AppendEntriesResponse handle_append_entries(const AppendEntriesRequest &req);

    // 接收RPC消息
    void receive_rpc(int from_node_id, RaftMessageType msg_type, const std::string &data);

    // 提交命令（返回日志索引，成功则>=0，失败则为-1）
    int submit_command(const std::string &cmd);

    // 获取当前角色
    RaftRole get_role() const { return role_.load(); }
};

#endif