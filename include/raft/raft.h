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
#include <future>
#include "common/base/export.h"
#include "raft/protocol/protocol.h"
#include "common/socket/cs/client.h"
#include "common/socket/cs/server.h"
#include "network/protocol/protocol.h"

//  Raft角色枚举
enum RaftRole
{
    Follower,
    Candidate,
    Leader
};

// 持久化状态结构
struct PersistentState
{
    std::atomic<uint32_t> current_term_{0};       // 当前任期
    std::string voted_for_ = "";                  // 本任期投票给的节点地址字符串
    ClusterMetadata cluster_metadata_;            // 集群配置
    std::atomic<uint32_t> log_snapshot_index_{0}; // 最后快照索引 (用于日志清理)
    std::atomic<uint32_t> commit_index_{0};       // 已提交的最高日志索引
    std::atomic<uint32_t> last_applied_{0};       // 已应用到状态机的最高日志索引
    PersistentState() {}

    PersistentState(const PersistentState &other)
    {
        current_term_.store(other.current_term_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        voted_for_ = other.voted_for_;
        cluster_metadata_ = other.cluster_metadata_;
        log_snapshot_index_.store(other.log_snapshot_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        commit_index_.store(other.commit_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_applied_.store(other.last_applied_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    PersistentState &operator=(const PersistentState &other)
    {
        current_term_.store(other.current_term_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        voted_for_ = other.voted_for_;
        cluster_metadata_ = other.cluster_metadata_;
        log_snapshot_index_.store(other.log_snapshot_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        commit_index_.store(other.commit_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_applied_.store(other.last_applied_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    // 序列化到文件
    std::string serialize() const
    {
        uint32_t current_term = current_term_.load(std::memory_order_relaxed);
        uint32_t log_snapshot_index = log_snapshot_index_.load(std::memory_order_relaxed);
        uint32_t commit_index = commit_index_.load(std::memory_order_relaxed);
        uint32_t last_applied = last_applied_.load(std::memory_order_relaxed);
        std::string data;
        data.append(reinterpret_cast<const char *>(&current_term), sizeof(current_term));
        data.append(Serializer::serialize(voted_for_));
        data.append(cluster_metadata_.serialize());
        data.append(reinterpret_cast<const char *>(&log_snapshot_index), sizeof(log_snapshot_index));
        data.append(reinterpret_cast<const char *>(&commit_index), sizeof(commit_index));
        data.append(reinterpret_cast<const char *>(&last_applied), sizeof(last_applied));
        return data;
    }

    // 从文件反序列化
    static PersistentState deserialize(const char *data, size_t &offset)
    {
        PersistentState state;
        uint32_t current_term, log_snapshot_index, commit_index, last_applied;
        std::memcpy(&current_term, data + offset, sizeof(current_term));
        offset += sizeof(current_term);
        state.voted_for_ = Serializer::deserializeString(data, offset);
        state.cluster_metadata_ = ClusterMetadata::deserialize(data, offset);
        std::memcpy(&log_snapshot_index, data + offset, sizeof(log_snapshot_index));
        offset += sizeof(log_snapshot_index);
        std::memcpy(&commit_index, data + offset, sizeof(commit_index));
        offset += sizeof(commit_index);
        std::memcpy(&last_applied, data + offset, sizeof(last_applied));
        offset += sizeof(last_applied);
        state.current_term_.store(current_term, std::memory_order_relaxed);
        state.log_snapshot_index_.store(log_snapshot_index, std::memory_order_relaxed);
        state.commit_index_.store(commit_index, std::memory_order_relaxed);
        state.last_applied_.store(last_applied, std::memory_order_relaxed);
        return state;
    }
};

class EYAKV_RAFT_API RaftLogArray
{
private:
    // 内存日志
    std::vector<LogEntry> entries_;
    uint32_t base_index_;             // 基准索引（最老日志索引）
    mutable std::shared_mutex mutex_; // 读写锁

    // WAL 文件 (Append-only, 顺序写)
    FILE *wal_file_ = nullptr;
    std::string wal_path_;

    // 索引文件 (记录每个日志条目的文件偏移量)
    std::vector<uint64_t> index_offsets_; // index_offsets_[i] = entries_[i] 在 WAL 中的偏移
    FILE *index_file_ = nullptr;
    std::string index_path_;

    // 元数据
    std::string log_dir_;

public:
    explicit RaftLogArray(const std::string &log_dir);
    ~RaftLogArray();

    // 追加日志 (内存 + WAL + 更新索引)
    bool append(LogEntry &entry);

    bool append(const LogEntry &entry);
    // 批量追加日志
    bool batch_append(std::vector<LogEntry> &entries);
    bool batch_append(const std::vector<LogEntry> &entries);
    // 获取指定索引的日志
    bool get(uint32_t index, LogEntry &entry) const;

    // 截断日志 (从某索引开始删除，模拟 Compaction)
    // truncate_from(index): 删除 index 及之后的日志 (保留 [base_index_, index))
    bool truncate_from(uint32_t index);

    // 截断日志 (在某索引之前删除，用于快照清理)
    // truncate_before(index): 删除 index 之前的日志 (保留 [index, last_index])
    bool truncate_before(uint32_t index);

    // 获取最后一条日志
    bool get_last(LogEntry &entry) const;

    // 获取最后一条日志的索引和任期
    bool get_last_info(uint32_t &index, uint32_t &term) const;

    // 获取日志数量
    uint32_t size() const;

    // 获取最后的日志索引
    uint32_t get_last_index() const;

    // 获取基准索引（最老的日志索引）
    uint32_t get_base_index() const;

    // 获取从指定索引开始的所有日志 (用于快照同步)
    std::vector<LogEntry> get_entries_from(uint32_t start_index) const;
    std::vector<LogEntry> get_entries_from(uint32_t start_index, int max_count) const;
    // 从磁盘恢复 (Recover 机制，使用 Index 加速)
    bool recover();

    // 创建快照点 (记录 snapshot_index，后续可 truncate 之前的日志)
    bool create_snapshot(uint32_t snapshot_index);

    // 刷盘 (强制写入磁盘)
    void flush();

    // 获取指定索引的日志任期
    uint32_t get_term(uint32_t index) const;

    // 同步元数据到磁盘
    void sync_metadata();

private:
    // 内部辅助方法
    bool write_entry_to_wal(const LogEntry &entry, uint64_t &offset); // 写入 WAL
    bool write_batch_to_wal(const std::vector<LogEntry> &entries);    // 批量写入 WAL
    bool write_index_entry(uint64_t offset);                          // 写入索引项
    bool read_entry_from_wal(uint64_t offset, LogEntry &entry) const; // 从 WAL 读取
    bool append_to_index(uint64_t offset);                            // 追加到内存索引
    void truncate_wal_and_index();                                    // 截断 WAL 和索引文件 (重建文件)
    bool load_index();                                                // 加载索引
    bool load_entries();                                              // 加载日志条目
    uint32_t compute_checksum(const std::string &data) const;         // 计算校验和
};

//  Raft节点类
class EYAKV_RAFT_API RaftNode : public TCPServer
{
private:
    RaftLogArray log_array_; // 日志数组

    // 持久化状态对象
    PersistentState persistent_state_; // 持久化状态

    //  易失性状态
    std::atomic<RaftRole> role_; // 角色

    //  易失性状态（仅 Leader）
    std::unordered_map<socket_t, uint32_t> next_index_;  // 每个Follower下一个要发送的日志索引
    std::unordered_map<socket_t, uint32_t> match_index_; // 每个Follower已匹配的日志索引

    // 受信任的ip+端口(应该支持模糊匹配)
    std::unordered_set<std::string> trusted_nodes_;

    //  超时配置（毫秒）
    int election_timeout_min_ = 150; // 选举超时最小值
    int election_timeout_max_ = 300; // 选举超时最大值
    int heartbeat_interval_ = 30;    // 心跳间隔
    int raft_msg_timeout_ = 1000;    // raft消息超时
    //  随机数生成（用于选举超时）
    std::mt19937 rng_;
    int election_timeout_;
    std::chrono::steady_clock::time_point last_heartbeat_time_;

    //  互斥锁（保证多线程安全）
    mutable std::mutex mutex_;

    //  线程控制
    std::thread election_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> election_thread_running_{false};
    std::atomic<bool> heartbeat_thread_running_{false};

    // 连接管理（与主节点的连接，如果是主节点则为nullptr）
    std::unique_ptr<TCPClient> follower_client_;
    std::thread follower_client_thread_;
    std::atomic<bool> follower_client_thread_running_{false};

    // 元数据文件句柄
    FILE *metadata_file_ = nullptr;
    std::string root_dir_; // 数据根目录

    // 是否允许写（主节点有效，从节点本身不可写）
    std::atomic<bool> writable{true};
    // 单例
    static std::unique_ptr<RaftNode> instance_;
    static bool is_init_;

    // 存储从节点socket(已初始化)
    std::unordered_set<socket_t> follower_sockets_;
    std::shared_mutex follower_sockets_mutex_;
    std::unordered_map<Address, socket_t> follower_address_map_;

    struct PendingRequest
    {
        std::promise<Response> promise;
        std::future<Response> future;

        PendingRequest()
        {
            future = promise.get_future();
        }
    };

    // 映射 Log Index -> PendingRequest
    // 当日志被应用时，通过这个 map 找到对应的 promise 通知 submit_command 返回
    std::unordered_map<uint32_t, std::shared_ptr<PendingRequest>> pending_requests_;
    std::mutex pending_requests_mutex_;

    struct SnapshotTransferState
    {
        uint64_t offset = 0;
        bool is_sending = false;
        uint32_t log_last_applied = 0;
        FILE *fp = nullptr; 
    };
    std::unordered_map<socket_t, SnapshotTransferState> snapshot_state_;

    // 辅助方法：通知等待的请求
    void notify_request_applied(uint32_t index, const Response &response);

    RaftNode(const std::string root_dir, const std::string &ip, const u_short port, const std::unordered_set<std::string> &trusted_nodes = {}, const uint32_t max_follower_count = 3);
    ProtocolBody *new_body()
    {
        return new RaftMessage();
    }

    void handle_request(ProtocolBody *body, socket_t client_sock) override;
    void close_socket(socket_t sock) override;
    // 初始化相关方法
    void load_persistent_state();       // 加载持久化状态
    void save_persistent_state();       // 保存持久化状态
    void init_as_follower();            // 场景1: 作为follower启动
    void init_with_cluster_discovery(); // 场景3: 探查集群
    void start_background_threads();    // 启动后台线程

    // 消息处理方法
    bool handle_append_entries(const RaftMessage &msg);                                // 处理AppendEntries请求
    bool handle_request_vote(const RaftMessage &msg, const socket_t &sock);            // 处理RequestVote请求
    void handle_append_entries_response(const RaftMessage &msg, const socket_t &sock); // 处理AppendEntries响应
    void handle_request_vote_response(const RaftMessage &msg, const socket_t &sock);   // 处理RequestVote响应
    void handle_query_leader(const RaftMessage &msg, const socket_t &client_sock);
    void handle_query_leader_response(const RaftMessage &msg);
    void handle_join_cluster(const RaftMessage &msg, const socket_t &client_sock); // 处理新节点加入请求
    void trigger_log_sync(socket_t sock);
    void send_snapshot_chunk(socket_t sock);
    void handle_install_snapshot(const RaftMessage &msg);
    void handle_join_cluster_response(const RaftMessage &msg);
    void handle_snapshot_response(const RaftMessage &msg, socket_t sock);
    // 日志复制和提交方法
    void send_append_entries(const socket_t &sock); // 发送AppendEntries
    void send_heartbeat_to_all();                   // 向所有节点发送心跳
    void send_request_vote();                       // 发送RequestVote
    void handle_append_entries_response(const std::string &follower, bool success,
                                        uint32_t conflict_index, uint32_t match_index); // 处理响应
    void try_commit_entries();                                                          // 尝试提交日志
    void commit_entry(uint32_t index);                                                  // 提交指定索引的日志
    void apply_committed_entries();                                                     // 应用已提交的日志到状态机

    // 辅助方法
    uint32_t get_current_timestamp(); // 获取当前时间戳
    int count_reachable_nodes();      // 计算可达节点数
    bool stop_accepting_writes();     // 停止接受写请求
    bool start_accepting_writes();
    // 广播消息到所有从节点
    void broadcast_to_followers(const RaftMessage &msg);

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
    // Follower客户端线程主循环
    void follower_client_loop();
    void handle_leader_message(const RaftMessage &msg);
    void handle_new_node_join(const RaftMessage &msg);
    void handle_leave_node(const RaftMessage &msg);
    // 转换为Follower
    bool become_follower(const Address &leader_addr, uint32_t term = 0, bool is_reconnect = false, const uint32_t commit_index = 0);

    // 转换为Candidate
    void become_candidate();

    // 转换为Leader
    void become_leader();

    // 连接到主节点
    bool connect_to_leader(const Address &leader_addr);
    // 发送RequestVote请求
    void send_request_vote();

    // 应用已提交的日志到状态机
    void apply_committed_entries();

    // 执行命令
    Response execute_command(const std::string &cmd);

    Response handle_raft_command(const std::vector<std::string> &command_parts, bool &is_exec);

    void add_new_connection(socket_t client_sock, const sockaddr_in &client_addr) override;

    bool remove_node(const Address &addr);

    bool is_trust(const Address &addr);

public:
    ~RaftNode();

    // 提交命令
    Response submit_command(const std::string &cmd);

    // 获取当前角色
    RaftRole get_role() const { return role_.load(); }

    static RaftNode *get_instance()
    {
        return instance_.get();
    }
    static bool is_init()
    {
        return is_init_;
    }

    static void init(const std::string root_dir, const std::string &ip, const u_short port, const std::unordered_set<std::string> &trusted_nodes = {}, const uint32_t max_follower_count = 5)
    {
        if (is_init_)
        {
            throw std::runtime_error("RaftNode already initialized");
        }
        instance_ = std::make_unique<RaftNode>(root_dir, ip, port, max_follower_count);
    }
};

#endif