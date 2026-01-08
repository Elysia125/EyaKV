#ifndef EYAKV_RAFT_RAFT_H_
#define EYAKV_RAFT_RAFT_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include "common/base/export.h"

#define LOG_SIZE 8096
// 日志条目结构体
struct LogEntry
{
    int term;        // 日志所属任期
    std::string cmd; // 客户端命令
    LogEntry(int t, const std::string &c) : term(t), cmd(c) {}
};

enum RaftRole
{
    Leader,
    Follower,
    Candidate,
};

class EYAKV_RAFT_API RaftNode
{
private:
    // 持久化状态
    int node_id_;                // 节点唯一ID
    uint32_t current_term_;      // 当前任期
    int voted_for_;              // 本任期投票给的节点（-1表示未投票）
    std::vector<LogEntry> logs_; // 日志列表(固定大小)
    uint32_t round_ = 0;         // 当前轮次(用于日志,轮次相同增量复制，轮次不同全量复制)
    FILE *meta_file_;            // 元数据文件
    // 易失性状态
    RaftRole role_; // 角色

    // 易失性状态（仅 Leader）
    std::unordered_map<int, uint32_t> match_index_; // 每个Follower已匹配的日志索引

    // 集群配置
    std::vector<int> cluster_nodes_; // 集群所有节点ID
    int leader_id_;                  // 当前感知的Leader ID（-1表示无）

    // 超时配置（毫秒）
    int election_timeout_min_ = 150; // 选举超时最小值
    int election_timeout_max_ = 300; // 选举超时最大值
    int heartbeat_interval_ = 30;    // 心跳间隔

    // 随机数生成（用于选举超时）
    std::mt19937 rng_;
    int election_timeout_;                                      // 本次选举超时时间
    std::chrono::steady_clock::time_point last_heartbeat_time_; // 最后一次收到心跳的时间

    // 互斥锁（保证多线程安全）
    std::mutex mutex_;

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

public:
    RaftNode() = default;
    ~RaftNode() = default;

    void start();
    void stop();
};

#endif