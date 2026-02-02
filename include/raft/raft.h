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
#include "common/ds/lru_cache.h"
#include "common/concurrency/threadpool.h"
#include "logger/logger.h"
// Raft角色枚举
enum RaftRole
{
    Follower,  // 跟随者：被动地接收Leader的日志复制请求
    Candidate, // 候选者：正在竞选Leader，请求其他节点投票
    Leader     // 领导者：负责处理客户端请求、日志复制和提交
};

/// @brief 将RaftRole转换为字符串
/// @param role Raft角色
/// @return 角色名称字符串
inline const char *role_to_string(RaftRole role)
{
    switch (role)
    {
    case RaftRole::Follower:
        return "Follower";
    case RaftRole::Candidate:
        return "Candidate";
    case RaftRole::Leader:
        return "Leader";
    default:
        return "Unknown";
    }
}

// 持久化状态结构：存储在稳定存储上的Raft节点状态
struct PersistentState
{
    std::atomic<uint32_t> current_term_{0};       // 当前任期：节点首次投票时递增，节点发现更高任期时更新
    Address voted_for_;                           // 本任期投票给的节点地址字符串：当前任期获得的选票的候选人ID
    ClusterMetadata cluster_metadata_;            // 集群配置：包含集群成员信息和配置版本
    std::atomic<uint32_t> log_snapshot_index_{0}; // 最后快照索引 (用于日志清理)：最新快照包含的日志索引
    std::atomic<uint32_t> commit_index_{0};       // 已提交的最高日志索引：已知已提交的最大日志索引
    std::atomic<uint32_t> last_applied_{0};       // 已应用到状态机的最高日志索引：已应用到状态机的最大日志索引

    /// @brief 默认构造函数
    PersistentState() {}

    /// @brief 拷贝构造函数：原子性复制所有持久化状态
    /// @param other 源状态对象
    PersistentState(const PersistentState &other)
    {
        current_term_.store(other.current_term_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        voted_for_ = other.voted_for_;
        cluster_metadata_ = other.cluster_metadata_;
        log_snapshot_index_.store(other.log_snapshot_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        commit_index_.store(other.commit_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_applied_.store(other.last_applied_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    /// @brief 赋值运算符：原子性复制所有持久化状态
    /// @param other 源状态对象
    /// @return 当前对象的引用
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

    /// @brief 序列化到文件：将持久化状态转换为二进制格式以进行持久化
    /// @return 序列化后的二进制字符串
    std::string serialize() const
    {
        uint32_t current_term = current_term_.load(std::memory_order_relaxed);
        uint32_t log_snapshot_index = log_snapshot_index_.load(std::memory_order_relaxed);
        uint32_t commit_index = commit_index_.load(std::memory_order_relaxed);
        uint32_t last_applied = last_applied_.load(std::memory_order_relaxed);
        std::string data;
        data.append(reinterpret_cast<const char *>(&current_term), sizeof(current_term));
        data.append(voted_for_.serialize());
        data.append(cluster_metadata_.serialize());
        data.append(reinterpret_cast<const char *>(&log_snapshot_index), sizeof(log_snapshot_index));
        data.append(reinterpret_cast<const char *>(&commit_index), sizeof(commit_index));
        data.append(reinterpret_cast<const char *>(&last_applied), sizeof(last_applied));
        return data;
    }

    /// @brief 从文件反序列化：从二进制数据恢复持久化状态
    /// @param data 二进制数据缓冲区
    /// @param offset 数据偏移量，会在函数中递增
    /// @return 反序列化后的持久化状态对象
    static PersistentState deserialize(const char *data, size_t &offset)
    {
        PersistentState state;
        uint32_t current_term, log_snapshot_index, commit_index, last_applied;
        std::memcpy(&current_term, data + offset, sizeof(current_term));
        offset += sizeof(current_term);
        state.voted_for_ = Address::deserialize(data, offset);
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

/// @brief Raft日志数组：管理Raft日志的内存缓存、WAL（Write-Ahead Log）持久化和索引
///
/// 提供日志的追加、查询、截断、恢复等功能，使用内存缓存+磁盘持久化的双层架构，
/// WAL文件顺序写入，索引文件记录偏移量以加速随机读取
class EYAKV_RAFT_API RaftLogArray
{
private:
    // 内存日志缓存
    std::vector<LogEntry> entries_;   // 日志条目数组，entries_[i] 对应逻辑索引 base_index_ + i
    uint32_t base_index_;             // 基准索引：entries_[0] 对应的逻辑索引（最老日志索引）
    mutable std::shared_mutex mutex_; // 读写锁：保护 entries_、base_index_、index_offsets_ 的并发访问

    // WAL 文件 (Write-Ahead Log, Append-only, 顺序写)
    FILE *wal_file_ = nullptr; // WAL文件句柄
    std::string wal_path_;     // WAL文件路径

    // 索引文件 (记录每个日志条目在WAL中的文件偏移量，加速随机读取)
    std::vector<uint64_t> index_offsets_; // index_offsets_[i] = entries_[i] 在 WAL 中的偏移量
    FILE *index_file_ = nullptr;          // 索引文件句柄
    std::string index_path_;              // 索引文件路径

    // 元数据
    std::string log_dir_; // 日志目录：存储WAL、索引文件和快照的根目录

public:
    /// @brief 构造函数：初始化日志数组
    /// @param log_dir 日志存储目录路径
    explicit RaftLogArray(const std::string &log_dir);

    /// @brief 析构函数：关闭文件句柄，释放资源
    ~RaftLogArray();

    /// @brief 追加日志条目（非const版本）
    /// @param entry 要追加的日志条目（引用传递，可被修改）
    /// @return 成功返回true，失败返回false
    bool append(LogEntry &entry);

    /// @brief 追加日志条目（const版本）
    /// @param entry 要追加的日志条目
    /// @return 成功返回true，失败返回false
    bool append(const LogEntry &entry);

    /// @brief 批量追加日志条目（非const版本）
    /// @param entries 要追加的日志条目数组（引用传递，可被修改）
    /// @return 成功返回true，失败返回false
    bool batch_append(std::vector<LogEntry> &entries);

    /// @brief 批量追加日志条目（const版本）
    /// @param entries 要追加的日志条目数组
    /// @return 成功返回true，失败返回false
    bool batch_append(const std::vector<LogEntry> &entries);

    /// @brief 获取指定索引的日志条目
    /// @param index 日志索引（逻辑索引）
    /// @param entry 输出参数，返回日志条目
    /// @return 成功返回true，失败返回false
    bool get(uint32_t index, LogEntry &entry) const;

    /// @brief 截断日志：从指定索引开始删除（用于回滚或冲突解决）
    /// @param index 起始索引（保留 [base_index_, index)，删除 [index, last_index]）
    /// @return 成功返回true，失败返回false
    bool truncate_from(uint32_t index);

    /// @brief 截断日志：删除指定索引之前的日志（用于快照清理）
    /// @param index 起始索引（删除 [base_index_, index)，保留 [index, last_index]）
    /// @return 成功返回true，失败返回false
    bool truncate_before(uint32_t index);

    /// @brief 获取最后一条日志
    /// @param entry 输出参数，返回最后一条日志条目
    /// @return 成功返回true，失败返回false
    bool get_last(LogEntry &entry) const;

    /// @brief 获取最后一条日志的索引和任期
    /// @param index 输出参数，返回最后一条日志的索引
    /// @param term 输出参数，返回最后一条日志的任期
    /// @return 成功返回true，失败返回false
    bool get_last_info(uint32_t &index, uint32_t &term) const;

    /// @brief 获取日志数量
    /// @return 日志条目数量
    uint32_t size() const;

    /// @brief 获取最后的日志索引
    /// @return 最后一条日志的逻辑索引
    uint32_t get_last_index() const;

    /// @brief 获取基准索引（最老的日志索引）
    /// @return 第一条日志的逻辑索引
    uint32_t get_base_index() const;

    /// @brief 获取从指定索引开始的所有日志（用于快照同步或日志复制）
    /// @param start_index 起始索引
    /// @return 从start_index开始到最后的所有日志条目
    std::vector<LogEntry> get_entries_from(uint32_t start_index) const;

    /// @brief 获取从指定索引开始的指定数量日志（用于批量日志复制）
    /// @param start_index 起始索引
    /// @param max_count 最大返回条目数
    /// @return 从start_index开始的最多max_count条日志条目
    std::vector<LogEntry> get_entries_from(uint32_t start_index, int max_count) const;

    /// @brief 从磁盘恢复日志（Recover机制，使用索引加速）
    /// @return 成功返回true，失败返回false
    bool recover();

    /// @brief 创建快照点：记录快照索引，后续可删除之前的旧日志
    /// @param snapshot_index 快照索引
    /// @return 成功返回true，失败返回false
    bool create_snapshot(uint32_t snapshot_index);

    /// @brief 刷盘：强制将WAL缓冲区内容写入磁盘
    void flush();

    /// @brief 获取指定索引的日志任期
    /// @param index 日志索引
    /// @return 日志对应的任期，如果索引无效返回0
    uint32_t get_term(uint32_t index) const;

    /// @brief 同步元数据到磁盘
    void sync_metadata();

    /// @brief 重置日志：设置新的起始索引（用于快照恢复后重建日志）
    /// @param new_start_index 新的起始索引
    /// @return 成功返回true，失败返回false
    bool reset(uint32_t new_start_index);

private:
    /// @brief 内部辅助方法：将日志条目写入WAL
    /// @param entry 日志条目
    /// @param offset 输出参数，返回写入位置在WAL中的偏移量
    /// @return 成功返回true，失败返回false
    bool write_entry_to_wal(const LogEntry &entry, uint64_t &offset);

    /// @brief 内部辅助方法：批量写入日志条目到WAL
    /// @param entries 日志条目数组
    /// @return 成功返回true，失败返回false
    bool write_batch_to_wal(const std::vector<LogEntry> &entries);

    /// @brief 内部辅助方法：写入索引项到索引文件
    /// @param offset 日志条目在WAL中的偏移量
    /// @return 成功返回true，失败返回false
    bool write_index_entry(uint64_t offset);

    /// @brief 内部辅助方法：从WAL读取日志条目
    /// @param offset 日志条目在WAL中的偏移量
    /// @param entry 输出参数，返回读取的日志条目
    /// @return 成功返回true，失败返回false
    bool read_entry_from_wal(uint64_t offset, LogEntry &entry) const;

    /// @brief 内部辅助方法：追加偏移量到内存索引
    /// @param offset 日志条目在WAL中的偏移量
    /// @return 成功返回true，失败返回false
    bool append_to_index(uint64_t offset);

    /// @brief 内部辅助方法：截断WAL和索引文件（重建文件）
    void truncate_wal_and_index();

    /// @brief 内部辅助方法：加载索引文件
    /// @return 成功返回true，失败返回false
    bool load_index();

    /// @brief 内部辅助方法：加载日志条目
    /// @return 成功返回true，失败返回false
    bool load_entries();

    /// @brief 内部辅助方法：计算校验和
    /// @param data 数据字符串
    /// @return 校验和值
    uint32_t compute_checksum(const std::string &data) const;
};

/// @brief Raft节点类：实现Raft一致性算法的节点，继承自TCPServer
///
/// 支持Leader选举、日志复制、快照传输、集群成员变更等Raft核心功能
/// 使用单例模式，通过静态方法init()和get_instance()访问
class EYAKV_RAFT_API RaftNode : public TCPServer
{
private:
    RaftLogArray log_array_; // 日志数组：管理Raft日志的存储和访问

    // 持久化状态对象
    PersistentState persistent_state_; // 持久化状态：包含current_term、voted_for等需要持久化的状态

    // 易失性状态
    std::atomic<RaftRole> role_; // 当前角色：Follower、Candidate或Leader

    // 易失性状态（仅 Leader 使用）
    std::unordered_map<socket_t, uint32_t> next_index_;  // 每个Follower下一个要发送的日志索引
    std::unordered_map<socket_t, uint32_t> match_index_; // 每个Follower已匹配的日志索引（已知已复制的最高日志索引）

    // 受信任的ip+端口（用于节点认证，支持模糊匹配）
    std::unordered_set<std::string> trusted_nodes_; // 受信任节点集合：只接受来自这些节点的Raft消息

    // 超时配置（毫秒）
    int election_timeout_min_ = 150; // 选举超时最小值：150ms
    int election_timeout_max_ = 300; // 选举超时最大值：300ms
    int heartbeat_interval_ = 30;    // 心跳间隔：30ms，Leader向Follower发送心跳的周期
    int raft_msg_timeout_ = 1000;    // Raft消息超时：1000ms，等待消息响应的超时时间

    // 随机数生成（用于选举超时）
    std::mt19937 rng_;                                          // 随机数生成器：用于生成随机选举超时
    int election_timeout_;                                      // 当前选举超时值：在[election_timeout_min_, election_timeout_max_]范围内随机
    std::chrono::steady_clock::time_point last_heartbeat_time_; // 最后心跳时间：用于检测选举超时

    // 互斥锁（保证多线程安全）
    mutable std::mutex mutex_; // 互斥锁：保护易失性状态的并发访问

    // 线程控制
    std::thread election_thread_;                       // 选举线程：负责选举超时检测和发起选举
    std::thread heartbeat_thread_;                      // 心跳线程：Leader向Follower发送心跳和日志复制
    std::atomic<bool> election_thread_running_{false};  // 选举线程运行标志
    std::atomic<bool> heartbeat_thread_running_{false}; // 心跳线程运行标志

    // 连接管理（与Leader的连接，如果是Leader则为nullptr）
    std::unique_ptr<TCPClient> follower_client_;              // Follower连接到Leader的TCP客户端
    std::thread follower_client_thread_;                      // Follower客户端线程：处理从Leader接收到的消息
    std::atomic<bool> follower_client_thread_running_{false}; // Follower客户端线程运行标志

    // 元数据文件句柄
    FILE *metadata_file_ = nullptr; // 元数据文件句柄：存储集群元数据
    std::string root_dir_;          // 数据根目录：存储日志、快照、元数据等的根目录

    // 是否允许写（仅Leader有效，Follower本身不可写）
    std::atomic<bool> writable_{true}; // 可写标志：只有Leader可以处理写请求

    // 单例模式
    static std::unique_ptr<RaftNode> instance_; // 单例实例指针
    static bool is_init_;                       // 是否已初始化标志

    // 存储从节点socket（已初始化的Follower连接）
    std::unordered_set<socket_t> follower_sockets_;              // Follower套接字集合：Leader维护的所有Follower连接
    std::shared_mutex follower_sockets_mutex_;                   // Follower套接字集合的读写锁
    std::unordered_map<Address, socket_t> follower_address_map_; // Follower地址到套接字的映射

    /// @brief 待处理的请求结构：用于异步等待日志被应用
    struct PendingRequest
    {
        std::promise<Response> promise; // Promise对象：用于通知日志应用结果
        std::future<Response> future;   // Future对象：用于等待日志应用结果

        PendingRequest()
        {
            future = promise.get_future();
        }
    };

    // 映射 Log Index -> PendingRequest
    // 当日志被应用时，通过这个 map 找到对应的 promise 通知 submit_command 返回
    std::unordered_map<uint32_t, std::shared_ptr<PendingRequest>> pending_requests_; // 待处理请求映射：日志索引 -> 请求
    std::mutex pending_requests_mutex_;                                              // 待处理请求映射的互斥锁

    /// @brief 快照传输状态结构
    struct SnapshotTransferState
    {
        uint64_t offset = 0;              // 当前传输偏移量
        bool is_sending = false;          // 是否正在发送
        FILE *fp = nullptr;               // 快照文件句柄
        std::string snapshot_path = "";   // 快照文件路径
        uint32_t last_included_index = 0; // 快照包含的最后日志索引
        uint32_t last_included_term = 0;  // 快照包含的最后日志任期
    };
    std::unordered_map<socket_t, SnapshotTransferState> snapshot_state_; // 快照传输状态映射：套接字 -> 传输状态

    // 命令缓存结果
    LRUCache<std::string, Response> result_cache_{10000}; // 结果缓存：LRU缓存已执行的命令结果，避免重复执行
    int granted_votes_ = 0;                               // 当前收到的赞成票数：用于选举时统计选票

    // 线程池
    std::unique_ptr<ThreadPool> thread_pool_; // 线程池：用于异步处理任务
    /// @brief 辅助方法：通知等待的请求（当日志被应用时调用）
    /// @param index 日志索引
    /// @param response 响应结果
    void notify_request_applied(uint32_t index, const Response &response);

    /// @brief 私有构造函数：创建Raft节点实例
    /// @param root_dir 数据根目录
    /// @param ip 本地IP地址
    /// @param port 本地端口
    /// @param trusted_nodes 受信任的节点集合
    /// @param max_follower_count 最大Follower数量
    RaftNode(const std::string root_dir, const std::string &ip, const u_short port, const std::unordered_set<std::string> &trusted_nodes = {}, const uint32_t max_follower_count = 3);

    /// @brief 创建Raft消息体
    /// @return 新的RaftMessage对象指针
    ProtocolBody *new_body()
    {
        return new RaftMessage();
    }

    /// @brief 处理接收到的请求（TCPServer接口实现）
    /// @param body 请求消息体
    /// @param client_sock 客户端套接字
    void handle_request(ProtocolBody *body, socket_t client_sock) override;

    /// @brief 关闭套接字（TCPServer接口实现）
    /// @param sock 要关闭的套接字
    void close_socket(socket_t sock) override;

    // 初始化相关方法
    /// @brief 加载持久化状态：从磁盘读取current_term、voted_for等
    void load_persistent_state();

    /// @brief 保存持久化状态：将current_term、voted_for等写入磁盘
    void save_persistent_state();

    /// @brief 初始化为Follower：连接到已知Leader或等待心跳
    void init_as_follower();

    /// @brief 初始化时探查集群：尝试发现并连接集群Leader
    void init_with_cluster_discovery();

    /// @brief 启动后台线程：启动选举线程和心跳线程
    void start_background_threads();

    // 消息处理方法
    /// @brief 处理AppendEntries请求（日志复制/心跳）
    /// @param msg Raft消息
    /// @return 成功返回true，失败返回false
    bool handle_append_entries(const RaftMessage &msg);

    /// @brief 处理RequestVote请求（投票）
    /// @param msg Raft消息
    /// @param sock 发送请求的套接字
    /// @return 成功返回true，失败返回false
    bool handle_request_vote(const RaftMessage &msg, const socket_t &sock);

    /// @brief 处理AppendEntries响应（Leader接收Follower的响应）
    /// @param msg Raft消息
    /// @param sock 发送响应的套接字
    void handle_append_entries_response(const RaftMessage &msg, const socket_t &sock);

    /// @brief 处理RequestVote响应（Candidate接收投票结果）
    /// @param msg Raft消息
    void handle_request_vote_response(const RaftMessage &msg);

    /// @brief 处理查询Leader请求
    /// @param msg Raft消息
    /// @param client_sock 客户端套接字
    void handle_query_leader(const RaftMessage &msg, const socket_t &client_sock);

    /// @brief 处理查询Leader响应
    /// @param msg Raft消息
    void handle_query_leader_response(const RaftMessage &msg);

    /// @brief 处理新节点加入请求
    /// @param msg Raft消息
    /// @param client_sock 客户端套接字
    void handle_join_cluster(const RaftMessage &msg, const socket_t &client_sock);

    /// @brief 处理InstallSnapshot请求（快照传输）
    /// @param msg Raft消息
    void handle_install_snapshot(const RaftMessage &msg);

    /// @brief 处理加入集群响应
    /// @param msg Raft消息
    void handle_join_cluster_response(const RaftMessage &msg);

    /// @brief 处理快照响应（Leader接收Follower的快照接收确认）
    /// @param msg Raft消息
    /// @param sock 发送响应的套接字
    void handle_snapshot_response(const RaftMessage &msg, socket_t sock);

    /// @brief 处理Leader消息（Follower接收Leader的命令）
    /// @param msg Raft消息
    void handle_leader_message(const RaftMessage &msg);

    /// @brief 处理新节点加入确认消息（Leader广播）
    /// @param msg Raft消息
    void handle_new_node_join(const RaftMessage &msg);

    /// @brief 处理节点离开消息
    /// @param msg Raft消息
    void handle_leave_node(const RaftMessage &msg);

    /// @brief 处理新Leader选举成功消息
    /// @param msg Raft消息
    /// @param client_sock 客户端套接字
    void handle_new_master(const RaftMessage &msg, const socket_t &client_sock);

    /// @brief 广播新Leader消息到所有已知节点
    void broadcast_new_master();

    /// @brief 处理AppendEntries响应（内部版本，用于更新next_index和match_index）
    /// @param follower Follower标识
    /// @param success 是否成功
    /// @param conflict_index 冲突索引
    /// @param match_index 匹配索引
    void handle_append_entries_response(const std::string &follower, bool success,
                                        uint32_t conflict_index, uint32_t match_index);

    // 日志复制和提交方法
    /// @brief 发送AppendEntries到指定Follower
    /// @param sock Follower套接字
    void send_append_entries(const socket_t &sock);

    /// @brief 向所有节点发送心跳（调用send_append_entries）
    void send_heartbeat_to_all();

    /// @brief 发送RequestVote请求（发起选举）
    void send_request_vote();

    /// @brief 发送快照数据块到Follower
    /// @param sock Follower套接字
    void send_snapshot_chunk(socket_t sock);

    // 日志同步方法
    /// @brief 触发日志同步（检查Follower是否需要同步日志）
    /// @param sock Follower套接字
    void trigger_log_sync(socket_t sock);

    /// @brief 尝试提交日志（检查是否有多数派副本已复制）
    void try_commit_entries();

    /// @brief 应用已提交的日志到状态机
    void apply_committed_entries();

    // 辅助方法
    /// @brief 获取当前时间戳（秒）
    /// @return 当前时间戳
    uint32_t get_current_timestamp();

    /// @brief 停止接受写请求（Follower或过渡期间）
    /// @return 成功返回true
    bool stop_accepting_writes();

    /// @brief 开始接受写请求（Leader）
    /// @return 成功返回true
    bool start_accepting_writes();

    /// @brief 广播消息到所有Follower
    /// @param msg 要广播的Raft消息
    void broadcast_to_followers(const RaftMessage &msg);

    /// @brief 生成随机选举超时时间
    /// @return 随机超时值（毫秒）
    int generate_election_timeout()
    {
        std::uniform_int_distribution<int> dist(election_timeout_min_, election_timeout_max_);
        return dist(rng_);
    }

    /// @brief 重置选举超时（更新最后心跳时间+重新生成超时值）
    void reset_election_timeout()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_heartbeat_time_ = std::chrono::steady_clock::now();
        election_timeout_ = generate_election_timeout();
    }

    /// @brief 检查是否触发选举超时
    /// @return 超时返回true，否则返回false
    bool is_election_timeout()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat_time_).count();
        return elapsed > election_timeout_;
    }

    /// @brief 选举线程主循环：检测超时并发起选举
    void election_loop();

    /// @brief 心跳线程主循环：Leader定时发送心跳
    void heartbeat_loop();

    /// @brief Follower客户端线程主循环：处理从Leader接收的消息
    void follower_client_loop();

    /// @brief 转换为Follower
    /// @param leader_addr Leader地址
    /// @param term 任期号（默认为0表示使用当前term）
    /// @param is_reconnect 是否是重连（重连时不重置某些状态）
    /// @param commit_index 提交索引（从Leader同步）
    /// @return 成功返回true
    bool become_follower(const Address &leader_addr, uint32_t term = 0, bool is_reconnect = false, const uint32_t commit_index = 0);

    /// @brief 转换为Candidate：开始选举
    void become_candidate();

    /// @brief 转换为Leader：选举成功后的初始化
    void become_leader();

    /// @brief 连接到Leader节点
    /// @param leader_addr Leader地址
    /// @return 成功返回true，失败返回false
    bool connect_to_leader(const Address &leader_addr);

    /// @brief 执行命令：将命令应用到状态机
    /// @param cmd 命令字符串
    /// @return 执行结果
    Response execute_command(const std::string &cmd);

    /// @brief 处理Raft命令（集群管理命令）
    /// @param command_parts 命令分段数组
    /// @param is_exec 输出参数，表示是否执行了命令
    /// @return 命令响应
    Response handle_raft_command(const std::vector<std::string> &command_parts, bool &is_exec);

    /// @brief 添加新连接（TCPServer接口实现）
    /// @param client_sock 客户端套接字
    /// @param client_addr 客户端地址
    void add_new_connection(socket_t client_sock, const sockaddr_in &client_addr) override;

    /// @brief 从集群中移除节点
    /// @param addr 要移除的节点地址
    /// @return 成功返回true，失败返回false
    bool remove_node(const Address &addr);

    /// @brief 检查地址是否在受信任节点列表中
    /// @param addr 要检查的地址
    /// @return 受信任返回true，否则返回false
    bool is_trust(const Address &addr);

public:
    /// @brief 析构函数：释放资源，停止后台线程
    ~RaftNode();

    /// @brief 提交命令：客户端调用，将命令提交到Raft日志
    /// @param request_id 请求唯一标识符（用于去重）
    /// @param cmd 命令字符串
    /// @return 命令执行结果（异步等待日志提交和应用）
    Response submit_command(const std::string &request_id, const std::string &cmd);

    /// @brief 获取当前角色
    /// @return 当前角色（Follower、Candidate或Leader）
    RaftRole get_role() const { return role_.load(); }

    /// @brief 获取节点ID（用于日志记录）
    /// @return 节点标识字符串，格式为 "ip:port"
    std::string get_node_id() const { return ip_ + ":" + std::to_string(port_); }

    /// @brief 获取当前任期号
    /// @return 当前任期号
    uint32_t get_current_term() const { return persistent_state_.current_term_.load(std::memory_order_relaxed); }

    /// @brief 获取当前Leader地址
    /// @return Leader地址，如果没有Leader则返回null地址
    Address get_leader_addr() const { return persistent_state_.cluster_metadata_.current_leader_; }

    /// @brief 获取已提交的日志索引
    /// @return 已提交的最高日志索引
    uint32_t get_commit_index() const { return persistent_state_.commit_index_.load(std::memory_order_relaxed); }

    /// @brief 获取已应用的日志索引
    /// @return 已应用到状态机的最高日志索引
    uint32_t get_last_applied() const { return persistent_state_.last_applied_.load(std::memory_order_relaxed); }

    /// @brief 获取RaftNode单例实例
    /// @return RaftNode指针，如果未初始化则返回nullptr
    static RaftNode *get_instance()
    {
        return instance_.get();
    }

    /// @brief 检查RaftNode是否已初始化
    /// @return 已初始化返回true，否则返回false
    static bool is_init()
    {
        return is_init_;
    }

    /// @brief 初始化RaftNode单例
    /// @param root_dir 数据根目录
    /// @param ip 本地IP地址
    /// @param port 本地端口
    /// @param trusted_nodes 受信任的节点集合
    /// @param max_follower_count 最大Follower数量（默认5）
    /// @throw std::runtime_error 如果已经初始化
    static void init(const std::string root_dir, const std::string &ip, const u_short port, const std::unordered_set<std::string> &trusted_nodes = {}, const uint32_t max_follower_count = 5)
    {
        if (is_init_)
        {
            throw std::runtime_error("RaftNode already initialized");
        }
        instance_ = std::unique_ptr<RaftNode>(new RaftNode(root_dir, ip, port, trusted_nodes, max_follower_count));
    }
};

#endif