# Raft分布式共识算法实现方案设计文档

## 文档版本信息
- 版本：v1.0
- 创建日期：2026-01-28
- 作者：TinyKV团队

---

## 1. 概述

### 1.1 设计目标
本方案旨在实现一个高性能、高可用的Raft分布式共识算法，满足以下核心需求：

- **线性一致性保证**：确保所有节点按相同顺序应用日志，提供强一致性
- **分区容错性**：在网络分区场景下保证系统可用性和数据一致性
- **运行效率优化**：通过存储优化、批量处理等机制提升吞吐量

### 1.2 核心改进点
相较于传统Raft实现，本方案在以下方面进行了优化：

| 改进项 | 原始方案 | 改进方案 | 改进理由 |
|--------|---------|---------|---------|
| 日志存储 | 环形缓冲区RaftLogRingBuffer | 无上限LogEntry数组 + 顺序写持久化 | 避免日志溢出，支持无限增长，提升持久化性能 |
| 初始化流程 | 简单的角色判断 | 四场景完整初始化逻辑 | 支持自举、重连、探查等多种启动场景 |
| 节点管理 | 基础连接管理 | 主动降级+全量同步+广播通知 | 简化节点加入流程，保证集群一致性 |
| 命令提交 | 单阶段提交 | 两阶段提交(预写+提交) | 保证分布式事务的ACID特性 |
| 故障恢复 | 基础选举 | 多数投票+强制覆盖+联合共识 | 防止脑裂，保证唯一leader |

---

## 2. 架构设计

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      RaftNode (节点实例)                      │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────────────────────┐  │
│  │  持久化状态      │  │        易失性状态               │  │
│  ├─────────────────┤  ├─────────────────────────────────┤  │
│  │ • current_term   │  │ • role (Follower/Candidate/Leader)│  │
│  │ • voted_for      │  │ • leader_address                │  │
│  │ • log_entries[]  │  │ • next_index[]                  │  │
│  │ • commit_index   │  │ • match_index[]                 │  │
│  │ • cluster_nodes  │  │ • last_applied                  │  │
│  └─────────────────┘  └─────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────────────────────┐  │
│  │  网络通信层      │  │        存储引擎层               │  │
│  ├─────────────────┤  ├─────────────────────────────────┤  │
│  │ • TCPServer     │  │ • LogFile (顺序写)              │  │
│  │ • TCPClient     │  │ • MetadataFile                 │  │
│  │ • 消息协议处理   │  │ • Storage (状态机)             │  │
│  └─────────────────┘  └─────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────────────────────┐  │
│  │  后台线程        │  │        定时任务                 │  │
│  ├─────────────────┤  ├─────────────────────────────────┤  │
│  │ • election_loop  │  │ • 选举超时检测                 │  │
│  │ • heartbeat_loop │  │ • 心跳发送 (Leader)            │  │
│  │ • apply_loop     │  │ • 日志应用 (所有节点)          │  │
│  └─────────────────┘  └─────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Raft集群 (3+ 节点)                       │
├─────────────────────────────────────────────────────────────┤
│  Node A (Leader)  ◄─────────────────────────────────►  Node B (Follower)
│         │                                             │
│         └─────────────────────────────────────────────►  Node C (Follower)
└─────────────────────────────────────────────────────────────┘
```

### 2.2 数据流向图

```
客户端请求
    │
    ▼
┌──────────────┐
│ Leader节点   │
└──────────────┘
    │
    ├───► 阶段1: 预写日志 (PreWrite)
    │       • 追加到本地LogEntry数组
    │       • 异步顺序写入Log文件
    │       • 返回日志索引给客户端
    │
    ├───► 阶段2: 复制日志 (Replicate)
    │       • 向所有Follower发送AppendEntries
    │       • 等待多数节点确认
    │       • 更新commit_index
    │
    └───► 阶段3: 提交执行 (Commit)
            • 应用已提交日志到状态机
            • 返回结果给客户端
```

---

## 3. 核心数据结构

### 3.1 日志条目 (LogEntry)

```cpp
// 日志条目定义
struct LogEntry {
    uint32_t index;          // 日志索引 (单调递增)
    uint32_t term;           // 任期号
    uint32_t command_type;   // 命令类型 (如 SET/DELETE 等)
    std::string command;     // 命令内容
    uint64_t timestamp;      // 时间戳 (用于调试和超时检测)

    // 序列化方法
    std::string serialize() const;
    static LogEntry deserialize(const char* data, size_t& offset);
};
```

**设计说明：**
- `index`：全局唯一，从1开始递增，用于定位日志位置
- `term`：记录日志所在的leader任期，用于选举时的日志完整性比较
- `command_type`：预留字段，可用于区分普通命令、配置变更等
- `timestamp`：辅助字段，用于故障排查和性能分析

### 3.2 无上限日志数组 (RaftLogArray)

```cpp
// [修改] 替换原有的RaftLogRingBuffer
class RaftLogArray {
private:
    std::vector<LogEntry> entries_;           // 无上限日志数组
    uint32_t base_index_;                     // 基准索引 (通常为1)
    mutable std::shared_mutex mutex_;          // 读写锁

    // 文件描述符 (顺序写)
    int log_fd_;                              // 日志文件描述符
    std::string log_file_path_;               // 日志文件路径

public:
    explicit RaftLogArray(const std::string& log_dir);
    ~RaftLogArray();

    // 追加日志 (内存+磁盘)
    bool append(const LogEntry& entry);

    // 获取指定索引的日志
    bool get(uint32_t index, LogEntry& entry) const;

    // 截断日志 (从某索引开始删除)
    bool truncate_from(uint32_t index);

    // 获取最后一条日志
    bool get_last(LogEntry& entry) const;

    // 获取最后一条日志的索引和任期
    bool get_last_info(uint32_t& index, uint32_t& term) const;

    // 获取日志数量
    uint32_t size() const;

    // 获取从指定索引开始的所有日志 (用于快照同步)
    std::vector<LogEntry> get_entries_from(uint32_t start_index) const;

    // 从磁盘加载日志 (启动时调用)
    bool load_from_disk();

    // 刷盘 (强制写入磁盘)
    void flush();

private:
    // 异步写入线程
    void async_write_thread();
    std::thread write_thread_;
    std::queue<LogEntry> write_queue_;
    std::atomic<bool> write_thread_running_;
};
```

**修改理由：**
1. **取消环形限制**：原`RaftLogRingBuffer`使用固定大小环形缓冲区，日志溢出时会丢弃旧数据。新方案使用`std::vector`支持无限增长。
2. **顺序写优化**：所有日志条目通过单个文件描述符顺序写入，避免随机I/O，大幅提升性能。
3. **持久化保证**：每次append后立即fsync，保证数据不丢失（可选配置异步刷盘）。
4. **内存管理**：内存中保留所有日志，磁盘作为持久化备份，可通过快照机制定期清理。

### 3.3 集群节点信息

```cpp
struct ClusterMetadata {
    std::vector<Address> cluster_nodes_;      // 集群所有节点地址列表
    Address current_leader_;                  // 当前leader地址 (可能为空)
    uint64_t cluster_version_;                // 集群配置版本号 (用于联合共识)

    std::string serialize() const;
    static ClusterMetadata deserialize(const char* data, size_t& offset);
};
```

### 3.4 节点持久化状态

```cpp
// 存储在 .raft_meta 文件中
struct PersistentState {
    uint32_t current_term_;                   // 当前任期
    Address voted_for_;                       // 本任期投票给的节点
    ClusterMetadata cluster_metadata_;        // 集群配置
    uint32_t log_snapshot_index_;             // 最后快照索引 (用于日志清理)
};
```

### 3.5 节点易失性状态

```cpp
// Leader专用状态
struct LeaderVolatileState {
    std::unordered_map<Address, uint32_t> next_index_;    // 下一个要发送的日志索引
    std::unordered_map<Address, uint32_t> match_index_;   // 已匹配的日志索引
};

// 所有节点共用的易失性状态
struct CommonVolatileState {
    RaftRole role_;                           // 当前角色
    uint32_t commit_index_;                   // 已提交的最高日志索引
    uint32_t last_applied_;                   // 已应用到状态机的最高日志索引
};
```

---

## 4. 初始化流程

### 4.1 四场景初始化决策树

```
启动RaftNode
    │
    ├─► 加载元数据文件 (.raft_meta)
    │       • current_term
    │       • voted_for
    │       • cluster_nodes
    │       • leader_address
    │
    ├─► 加载日志文件 (LogEntry数组)
    │
    └─► 判断启动场景
            │
            ├───► 场景1: leader_address 非空
            │           └─► 作为Follower启动
            │                   • 连接leader_address
            │                   • 同步集群配置
            │                   • 同步缺失日志
            │
            ├───► 场景2: leader_address 为空 && cluster_nodes 为空
            │           └─► 自举为初始Leader
            │                   • 设置role_ = Leader
            │                   • 生成首个快照
            │                   • 启动心跳线程
            │
            └─► 场景3: leader_address 为空 && cluster_nodes 非空
                        └─► 探查集群状态
                                • 随机选择一个节点连接
                                • 发送探查消息 (QueryLeader)
                                • 根据响应决定:
                                    • 有leader → 作为Follower加入
                                    • 无leader → 发起选举
```

### 4.2 初始化流程伪代码

```cpp
RaftNode::RaftNode(const std::string root_dir, ...) {
    // 1. 加载持久化状态
    load_persistent_state();

    // 2. 加载日志
    log_array_.load_from_disk();

    // 3. 根据场景选择初始化策略
    if (!leader_address_.is_null()) {
        // 场景1: 已知leader，作为follower加入
        init_as_follower();
    } else if (cluster_nodes_.empty()) {
        // 场景2: 首个节点，自举为leader
        init_as_bootstrap_leader();
    } else {
        // 场景3: 探查集群
        init_with_cluster_discovery();
    }

    // 4. 启动后台线程
    start_background_threads();
}

void RaftNode::init_as_follower() {
    role_ = RaftRole::Follower;
    reset_election_timeout();

    // 连接leader
    follower_client_ = std::make_unique<TCPClient>();
    if (!follower_client_->connect(leader_address_.ip, leader_address_.port)) {
        LOG_ERROR("Failed to connect to leader: %s", leader_address_.to_string());
        // 回退到场景3：探查集群
        init_with_cluster_discovery();
        return;
    }

    // 同步集群配置
    sync_cluster_metadata();

    // 同步缺失日志
    sync_missing_logs();
}

void RaftNode::init_as_bootstrap_leader() {
    role_ = RaftRole::Leader;
    current_term_ = 1;
    leader_address_ = Address(ip_, port_);

    // 初始化next_index和match_index
    for (const auto& node : cluster_nodes_) {
        next_index_[node] = log_array_.get_last_index() + 1;
        match_index_[node] = 0;
    }

    // 持久化状态
    save_persistent_state();

    // 启动心跳线程
    heartbeat_thread_running_ = true;
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
}

void RaftNode::init_with_cluster_discovery() {
    role_ = RaftRole::Follower;

    // 随机选择一个节点探查
    auto target_node = cluster_nodes_[rng_() % cluster_nodes_.size()];

    TCPClient client;
    if (!client.connect(target_node.ip, target_node.port)) {
        LOG_WARN("Failed to connect to %s, try another", target_node.to_string());
        // 尝试下一个节点
        // ...
        return;
    }

    // 发送探查消息
    QueryLeaderMsg query;
    client.send(query);

    // 等待响应
    QueryLeaderResponse response = client.receive();

    if (response.has_leader) {
        // 发现已有leader
        leader_address_ = response.leader_address;
        init_as_follower();
    } else {
        // 无leader，发起选举
        become_candidate();
    }
}
```

---

## 5. 节点管理与角色转换

### 5.1 角色状态转换图

```
                    ┌─────────┐
                    │ Follower│
                    └────┬────┘
                         │ election_timeout
                         ▼
                    ┌─────────┐
         win_votes  │Candidate│  discover_higher_term
          (majority)└────┬────┘
                         │           │
                         │           ▼
                         │      ┌─────────┐
                         └─────►│ Follower│
                                └─────────┘

                    ┌─────────┐
                    │  Leader │
                    └────┬────┘
                         │ discover_higher_term
                         ▼
                    ┌─────────┐
                    │ Follower│
                    └─────────┘
```

### 5.2 角色转换实现

#### 5.2.1 转换为Follower

```cpp
void RaftNode::become_follower(uint32_t term) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新任期
    if (term > current_term_) {
        current_term_ = term;
        voted_for_ = Address();  // 重置投票
        save_persistent_state();
    }

    // 更新角色
    role_ = RaftRole::Follower;

    // 清空leader状态
    next_index_.clear();
    match_index_.clear();

    // 重置选举超时
    reset_election_timeout();

    LOG_INFO("Became follower, term: %u", current_term_);
}
```

**一致性保证：**
- 发现更高term时立即降级，避免多个leader并存
- 重置`voted_for`，允许在当前term重新投票

#### 5.2.2 转换为Candidate

```cpp
void RaftNode::become_candidate() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 增加任期
    current_term_++;
    voted_for_ = Address(ip_, port_);  // 投给自己
    save_persistent_state();

    // 更新角色
    role_ = RaftRole::Candidate;

    // 初始化投票计数
    int vote_count = 1;  // 自己的一票
    int required_votes = cluster_nodes_.size() / 2 + 1;  // 多数

    // 发送RequestVote
    send_request_vote();

    // 等待投票结果 (通过回调或异步处理)
    // ...

    LOG_INFO("Became candidate, term: %u", current_term_);
}
```

**防活锁机制：**
- 每次选举随机超时：150ms ~ 300ms
- 如果在选举过程中收到更高term的AppendEntries，立即降级为Follower

#### 5.2.3 转换为Leader

```cpp
void RaftNode::become_leader() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新角色
    role_ = RaftRole::Leader;
    leader_address_ = Address(ip_, port_);

    // 初始化Leader专用状态
    uint32_t last_index = log_array_.get_last_index();
    for (const auto& node : cluster_nodes_) {
        if (node != leader_address_) {
            next_index_[node] = last_index + 1;
            match_index_[node] = 0;
        }
    }

    // 立即发送心跳
    send_heartbeat_to_all();

    // 启动心跳线程
    heartbeat_thread_running_ = true;
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);

    LOG_INFO("Became leader, term: %u", current_term_);
}
```

**线性一致性保证：**
- 新leader上任后立即发送心跳，防止其他节点超时
- 新leader必须包含所有已提交的日志（通过选举保证）

### 5.3 主动连接降级机制

```cpp
// [新增] 当节点主动连接到其他节点时，自动降级为Follower
void RaftNode::on_active_connect(const Address& target_address) {
    if (role_ == RaftRole::Leader) {
        LOG_WARN("Active connecting to another node, step down to follower");
        become_follower(current_term_);
    }

    // 尝试连接
    TCPClient client;
    if (client.connect(target_address.ip, target_address.port)) {
        // 发送探查消息
        QueryLeaderMsg query;
        client.send(query);

        QueryLeaderResponse response = client.receive();

        if (response.has_leader && response.term >= current_term_) {
            leader_address_ = response.leader_address;
            become_follower(response.term);
        }
    }
}
```

**设计理由：**
- 防止两个leader同时存在（脑裂）
- 简化节点加入流程，新节点主动连接后自动成为follower

---

## 6. 命令提交流程

### 6.1 两阶段提交设计

```
Client                    Leader                    Followers
  │                          │                            │
  │───► 1. Submit(cmd) ──────►                            │
  │                          │                            │
  │                          ├───► 2. PreWrite Log        │
  │                          │    (内存+磁盘)             │
  │                          │                            │
  │◄─── 3. Return(index) ────┤                            │
  │                          │                            │
  │                          │───► 4. AppendEntries ────►│
  │                          │    (批量发送)              │
  │                          │                            │
  │                          │◄─── 5. ACK ───────────────│
  │                          │                            │
  │                          │───► 6. Check majority      │
  │                          │                            │
  │                          │───► 7. Commit ────────────►│
  │                          │    (更新commit_index)     │
  │                          │                            │
  │                          │───► 8. Apply to state     │
  │                          │    machine                 │
  │                          │                            │
  │◄─── 9. Result ───────────┤                            │
```

### 6.2 两阶段提交实现

#### 阶段1: 预写日志 (PreWrite)

```cpp
Response RaftNode::submit_command(const std::string& cmd) {
    if (role_ != RaftRole::Leader) {
        return Response::error("Not the leader");
    }

    // 创建日志条目
    LogEntry entry;
    entry.index = log_array_.get_last_index() + 1;
    entry.term = current_term_;
    entry.command = cmd;
    entry.timestamp = get_current_timestamp();

    // [阶段1] 预写日志
    if (!log_array_.append(entry)) {
        return Response::error("Failed to append log");
    }

    // 返回日志索引 (客户端可用此索引查询结果)
    return Response::success(entry.index);
}
```

#### 阶段2: 提交执行 (Commit)

```cpp
// Leader收到多数ACK后调用
void RaftNode::commit_entry(uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index <= commit_index_) {
        return;  // 已提交
    }

    // 更新commit_index
    commit_index_ = index;

    // 应用到状态机
    apply_committed_entries();
}

void RaftNode::apply_committed_entries() {
    while (last_applied_ < commit_index_) {
        last_applied_++;

        // 获取日志条目
        LogEntry entry;
        if (log_array_.get(last_applied_, entry)) {
            // 应用到状态机
            Response result = execute_command(entry.command);

            // 记录应用结果 (可选持久化)
            LOG_INFO("Applied log %u, result: %s", last_applied_,
                    result.success ? "success" : "failed");
        }
    }
}
```

**一致性保证：**
- 只有在多数节点确认后才提交，确保日志不会回滚
- 所有节点按相同顺序应用日志，保证线性一致性

### 6.3 日志复制 (AppendEntries)

```cpp
void RaftNode::send_append_entries(const Address& follower, bool is_heartbeat) {
    uint32_t next_idx = next_index_[follower];

    // 获取待发送的日志
    std::vector<LogEntry> entries;
    if (!is_heartbeat) {
        entries = log_array_.get_entries_from(next_idx);
    }

    // 构造AppendEntries消息
    AppendEntriesMsg msg;
    msg.term = current_term_;
    msg.leader_id = Address(ip_, port_);
    msg.prev_log_index = next_idx - 1;
    msg.prev_log_term = log_array_.get_term(next_idx - 1);
    msg.entries = entries;
    msg.leader_commit = commit_index_;

    // 发送
    TCPClient client;
    if (client.connect(follower.ip, follower.port)) {
        client.send(msg);

        // 接收响应
        AppendEntriesResponse response = client.receive();

        handle_append_entries_response(follower, response);
    }
}

void RaftNode::handle_append_entries_response(const Address& follower,
                                               const AppendEntriesResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (response.term > current_term_) {
        // 发现更高term，降级
        become_follower(response.term);
        return;
    }

    if (!response.success) {
        // 复制失败，回退next_index
        if (response.conflict_index > 0) {
            next_index_[follower] = response.conflict_index;
        } else {
            next_index_[follower]--;
        }

        // 重试
        send_append_entries(follower, false);
    } else {
        // 复制成功，更新match_index和next_index
        match_index_[follower] = response.match_index;
        next_index_[follower] = match_index_[follower] + 1;

        // 检查是否可以提交
        try_commit_entries();
    }
}

void RaftNode::try_commit_entries() {
    // 找到大多数节点都已匹配的日志索引
    std::vector<uint32_t> matched_indices;
    for (const auto& [node, idx] : match_index_) {
        matched_indices.push_back(idx);
    }

    std::sort(matched_indices.begin(), matched_indices.end(),
              std::greater<uint32_t>());

    uint32_t majority_index = matched_indices[cluster_nodes_.size() / 2];

    // 只有当前term的日志才能提交
    if (majority_index > commit_index_ &&
        log_array_.get_term(majority_index) == current_term_) {
        commit_entry(majority_index);
    }
}
```

**优化点：**
- 批量发送：每次可发送多个日志条目，减少网络往返
- 快速回退：发现冲突时直接回退到冲突索引，而非逐条回退
- 异步复制：使用多个线程并发发送日志

---

## 7. 消息协议定义

### 7.1 消息类型枚举

```cpp
enum RaftMessageType {
    // Raft核心消息
    RequestVote,              // 选举投票
    RequestVoteResponse,
    AppendEntries,            // 日志复制/心跳
    AppendEntriesResponse,

    // [新增] 集群管理消息
    QueryLeader,              // 探查leader
    QueryLeaderResponse,
    ClusterConfig,            // 集群配置变更
    ClusterConfigResponse,
    JoinCluster,              // 新节点加入
    JoinClusterResponse,

    // [新增] 日志同步消息
    SnapshotInstall,          // 快照安装
    SnapshotInstallResponse,
    LogSyncRequest,           // 日志同步请求
    LogSyncResponse
};
```

### 7.2 RequestVote消息

```cpp
struct RequestVoteMsg {
    uint32_t term;                    // 候选人的任期号
    Address candidate_id;             // 候选人的地址
    uint32_t last_log_index;          // 候选人的最后一条日志索引
    uint32_t last_log_term;           // 候选人的最后一条日志任期

    std::string serialize() const;
    static RequestVoteMsg deserialize(const char* data, size_t& offset);
};

struct RequestVoteResponse {
    uint32_t term;                    // 当前任期号 (可能大于请求的term)
    bool vote_granted;                 // 是否投票

    std::string serialize() const;
    static RequestVoteResponse deserialize(const char* data, size_t& offset);
};
```

### 7.3 AppendEntries消息

```cpp
struct AppendEntriesMsg {
    uint32_t term;                    // leader的任期
    Address leader_id;                 // leader的地址
    uint32_t prev_log_index;          // 紧邻新日志条目之前的日志索引
    uint32_t prev_log_term;           // 紧邻新日志条目之前的日志任期
    std::vector<LogEntry> entries;    // 准备存储的日志条目 (可为空，表示心跳)
    uint32_t leader_commit;           // leader已提交的最高日志索引

    std::string serialize() const;
    static AppendEntriesMsg deserialize(const char* data, size_t& offset);
};

struct AppendEntriesResponse {
    uint32_t term;                    // 当前任期
    bool success;                      // 如果follower包含prev_log_index和term则为true
    uint32_t conflict_index;          // 冲突日志的索引 (0表示无冲突)
    uint32_t conflict_term;          // 冲突日志的任期
    uint32_t match_index;             // 已匹配的最高日志索引

    std::string serialize() const;
    static AppendEntriesResponse deserialize(const char* data, size_t& offset);
};
```

### 7.4 [新增] 集群管理消息

#### QueryLeader (探查leader)

```cpp
struct QueryLeaderMsg {
    std::string serialize() const;
    static QueryLeaderMsg deserialize(const char* data, size_t& offset);
};

struct QueryLeaderResponse {
    bool has_leader;                   // 是否存在leader
    Address leader_address;            // leader地址 (如果存在)
    uint32 leader_term;               // leader的任期

    std::string serialize() const;
    static QueryLeaderResponse deserialize(const char* data, size_t& offset);
};
```

#### JoinCluster (新节点加入)

```cpp
struct JoinClusterMsg {
    Address new_node;                 // 新加入节点的地址
    std::string password;             // 认证密码 (可选)

    std::string serialize() const;
    static JoinClusterMsg deserialize(const char* data, size_t& offset);
};

struct JoinClusterResponse {
    bool success;                      // 是否成功加入
    std::string error_message;         // 错误信息 (如果失败)
    ClusterMetadata cluster_metadata;  // 集群配置
    uint32_t sync_from_index;          // 需要从哪个索引开始同步日志

    std::string serialize() const;
    static JoinClusterResponse deserialize(const char* data, size_t& offset);
};
```

#### ClusterConfig (集群配置变更 - 联合共识)

```cpp
struct ClusterConfigMsg {
    uint32_t term;                    // leader的任期
    uint32_t old_cluster_version;     // 旧集群版本号
    uint32_t new_cluster_version;     // 新集群版本号
    std::vector<Address> old_nodes;    // 旧节点列表
    std::vector<Address> new_nodes;    // 新节点列表 (联合共识时包含两份)

    std::string serialize() const;
    static ClusterConfigMsg deserialize(const char* data, size_t& offset);
};

struct ClusterConfigResponse {
    bool success;                      // 是否接受配置变更
    uint32_t term;                     // 当前任期

    std::string serialize() const;
    static ClusterConfigResponse deserialize(const char* data, size_t& offset);
};
```

### 7.5 [新增] 快照同步消息

```cpp
struct SnapshotInstallMsg {
    uint32_t term;                    // leader的任期
    Address leader_id;                 // leader的地址
    uint32_t last_included_index;     // 快照包含的最后一条日志索引
    uint32_t last_included_term;      // 快照包含的最后一条日志任期
    std::string snapshot_data;        // 快照数据 (压缩)
    uint32_t offset;                  // 快照数据的偏移量 (分块传输)
    bool done;                         // 是否为最后一块

    std::string serialize() const;
    static SnapshotInstallMsg deserialize(const char* data, size_t& offset);
};

struct SnapshotInstallResponse {
    uint32_t term;                    // 当前任期
    bool success;                      // 是否成功

    std::string serialize() const;
    static SnapshotInstallResponse deserialize(const char* data, size_t& offset);
};
```

---

## 8. 异常处理流程

### 8.1 选举超时处理

```cpp
void RaftNode::election_loop() {
    while (election_thread_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (is_election_timeout()) {
            LOG_INFO("Election timeout, starting election");
            become_candidate();
        }
    }
}

bool RaftNode::is_election_timeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heartbeat_time_).count();
    return elapsed > election_timeout_;
}
```

**随机超时机制：**
- 每次重置超时时生成随机值：150ms ~ 300ms
- 避免多个节点同时超时，减少选票瓜分

### 8.2 日志冲突处理

**场景：** Follower的日志与Leader不一致

```cpp
bool RaftNode::handle_append_entries(const AppendEntriesMsg& msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 检查term
    if (msg.term < current_term_) {
        return false;
    }

    // 2. 发现更高term，降级为follower
    if (msg.term > current_term_) {
        become_follower(msg.term);
    }

    // 重置选举超时
    reset_election_timeout();

    // 3. 检查prev_log_index和term
    if (msg.prev_log_index > 0) {
        LogEntry prev_entry;
        if (!log_array_.get(msg.prev_log_index, prev_entry)) {
            // 找不到prev_log_index，返回冲突
            return false;
        }

        if (prev_entry.term != msg.prev_log_term) {
            // term不匹配，返回冲突
            return false;
        }
    }

    // 4. 追加新日志
    if (!msg.entries.empty()) {
        for (const auto& entry : msg.entries) {
            LogEntry existing_entry;
            if (log_array_.get(entry.index, existing_entry)) {
                // 已存在该索引的日志，检查term是否匹配
                if (existing_entry.term != entry.term) {
                    // term不匹配，删除冲突日志
                    log_array_.truncate_from(entry.index);
                }
            }
            // 追加新日志
            log_array_.append(entry);
        }
    }

    // 5. 更新commit_index (仅当leader_commit > commit_index_)
    if (msg.leader_commit > commit_index_) {
        uint32_t new_commit = std::min(msg.leader_commit,
                                       log_array_.get_last_index());
        commit_index_ = new_commit;
        apply_committed_entries();
    }

    return true;
}
```

**强制覆盖机制：**
- 发现冲突时，Leader强制覆盖Follower的日志
- 通过比较(prev_log_index, prev_log_term)定位冲突点
- 删除冲突日志后追加Leader的日志

### 8.3 网络分区处理

**场景：** 集群被划分为多数派和少数派

```
分区前:  [Leader A] —— [Follower B] —— [Follower C]
分区后:  [Leader A] —— [网络隔离] —— [Follower B] —— [Follower C]
结果:    (少数派)                       (多数派)
                                        Leader B (新选举)
```

**处理流程：**

1. **多数派 (B, C):**
   - B选举成为新Leader
   - 继续处理客户端请求
   - 保持日志一致性

2. **少数派 (A):**
   - 无法获得多数选票，保持Candidate或降级为Follower
   - 不处理写请求，但可读未提交数据（可选）
   - 网络恢复后，自动发现新Leader并同步日志

**代码实现：**

```cpp
void RaftNode::handle_network_partition() {
    // 检测到网络分区 (心跳超时)
    if (role_ == RaftRole::Leader) {
        // 检查是否属于少数派
        int reachable_nodes = count_reachable_nodes();
        int required = cluster_nodes_.size() / 2 + 1;

        if (reachable_nodes < required) {
            LOG_WARN("Detected minority partition, step down");
            // 降级为Follower
            become_follower(current_term_);
            // 停止处理写请求
            stop_accepting_writes();
        }
    }
}
```

### 8.4 脑裂防护 (联合共识)

**场景：** 配置变更时网络分区可能导致多个Leader

**解决方案：** 采用两阶段配置变更 (联合共识)

```
阶段1: Cold → Cnew (联合配置)
    集群配置: Cnew = Cold ∪ {新节点}
    日志条目中包含两份配置: (Cold, Cnew)
    决策需要Cold和Cnew中的多数派

阶段2: Cnew → Cnew' (新配置生效)
    集群配置: Cnew' = Cnew - {旧节点}
    日志条目中只包含新配置: Cnew'
    决策需要Cnew'中的多数派
```

**代码实现：**

```cpp
bool RaftNode::apply_cluster_config_change(const std::vector<Address>& new_nodes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 阶段1: 联合共识
    ClusterConfigMsg msg;
    msg.term = current_term_;
    msg.old_cluster_version = cluster_metadata_.cluster_version_;
    msg.new_cluster_version = cluster_metadata_.cluster_version_ + 1;
    msg.old_nodes = cluster_metadata_.cluster_nodes_;
    msg.new_nodes = new_nodes;  // 包含所有新旧节点

    // 计算所需的多数派
    int old_majority = msg.old_nodes.size() / 2 + 1;
    int new_majority = msg.new_nodes.size() / 2 + 1;
    int required_votes = std::max(old_majority, new_majority);

    // 发送联合共识配置
    int accepted_votes = 1;  // 自己
    for (const auto& node : msg.new_nodes) {
        if (node == Address(ip_, port_)) continue;

        ClusterConfigResponse response = send_cluster_config(node, msg);
        if (response.success && response.term == current_term_) {
            accepted_votes++;
        }
    }

    if (accepted_votes >= required_votes) {
        // 阶段2: 新配置生效
        cluster_metadata_.cluster_nodes_ = new_nodes;
        cluster_metadata_.cluster_version_ = msg.new_cluster_version;
        save_persistent_state();

        LOG_INFO("Cluster config change applied, version: %u",
                 cluster_metadata_.cluster_version_);
        return true;
    } else {
        LOG_ERROR("Failed to get majority votes for cluster config change");
        return false;
    }
}
```

**一致性保证：**
- 联合共识阶段需要新旧配置的多数派同时同意
- 避免配置变更期间出现多个Leader

---

## 9. 性能优化建议

### 9.1 存储优化

#### 9.1.1 顺序写优化

```cpp
// 使用O_DIRECT标志绕过页缓存
log_fd_ = open(log_file_path_.c_str(),
               O_WRONLY | O_CREAT | O_APPEND | O_DIRECT, 0644);

// 批量写入，减少fsync调用
void RaftLogArray::batch_append(const std::vector<LogEntry>& entries) {
    std::vector<char> buffer;
    for (const auto& entry : entries) {
        buffer.insert(buffer.end(),
                      entry.serialize().begin(),
                      entry.serialize().end());
    }

    // 一次性写入
    write(log_fd_, buffer.data(), buffer.size());

    // 定期fsync (而非每次fsync)
    if (++write_count_ >= sync_interval_) {
        fsync(log_fd_);
        write_count_ = 0;
    }
}
```

**优化效果：**
- 减少I/O次数：批量写入代替单条写入
- 减少fsync调用：定期刷盘代替每次刷盘
- 提升吞吐量：预计提升3-5倍

#### 9.1.2 快照机制

```cpp
// 定期创建快照，清理旧日志
void RaftNode::create_snapshot() {
    uint32_t last_applied = last_applied_;
    uint32_t snapshot_index = last_applied;

    // 序列化状态机
    std::string snapshot_data = storage_->serialize();

    // 压缩快照
    std::string compressed_snapshot = compress(snapshot_data);

    // 保存快照文件
    std::string snapshot_path = get_snapshot_path(snapshot_index);
    write_file(snapshot_path, compressed_snapshot);

    // 更新元数据
    persistent_state_.log_snapshot_index_ = snapshot_index;
    save_persistent_state();

    // 清理旧日志
    log_array_.truncate_before(snapshot_index + 1);
}
```

**优化效果：**
- 减少日志体积：定期清理已提交日志
- 加速恢复：新节点可直接从快照恢复
- 节省磁盘空间：快照压缩率通常>70%

### 9.2 网络优化

#### 9.2.1 批量日志复制

```cpp
// 每次复制最多1000条日志
#define MAX_BATCH_ENTRIES 1000

void RaftNode::send_append_entries(const Address& follower) {
    uint32_t next_idx = next_index_[follower];
    uint32_t last_idx = log_array_.get_last_index();

    // 计算批次大小
    uint32_t batch_size = std::min(last_idx - next_idx + 1,
                                   (uint32_t)MAX_BATCH_ENTRIES);

    std::vector<LogEntry> entries =
        log_array_.get_entries_from(next_idx, batch_size);

    AppendEntriesMsg msg;
    msg.term = current_term_;
    msg.leader_id = Address(ip_, port_);
    msg.prev_log_index = next_idx - 1;
    msg.prev_log_term = log_array_.get_term(next_idx - 1);
    msg.entries = entries;
    msg.leader_commit = commit_index_;

    // 发送
    send_message(follower, msg);
}
```

**优化效果：**
- 减少网络往返：一次传输多条日志
- 提升吞吐量：预计提升2-3倍

#### 9.2.2 并发复制

```cpp
// 使用线程池并发发送日志
void RaftNode::replicate_to_all_followers() {
    std::vector<std::future<void>> futures;

    for (const auto& follower : cluster_nodes_) {
        if (follower == Address(ip_, port_)) continue;

        // 提交任务到线程池
        futures.push_back(thread_pool_.submit([this, follower]() {
            send_append_entries(follower);
        }));
    }

    // 等待所有任务完成
    for (auto& future : futures) {
        future.wait();
    }
}
```

**优化效果：**
- 并发发送：多个follower同时复制
- 降低延迟：减少单节点成为瓶颈

### 9.3 内存优化

#### 9.3.1 分层日志缓存

```cpp
// L1: 最近1000条日志 (内存)
// L2: 最近10000条日志 (内存 + 索引)
// L3: 所有日志 (磁盘)

class LogCache {
private:
    std::deque<LogEntry> l1_cache_;              // L1缓存 (最近1000条)
    std::unordered_map<uint32_t, LogEntry> l2_cache_;  // L2缓存 (最近10000条)
    RaftLogArray l3_storage_;                    // L3存储 (磁盘)

public:
    bool get(uint32_t index, LogEntry& entry) {
        // 先查L1
        for (const auto& e : l1_cache_) {
            if (e.index == index) {
                entry = e;
                return true;
            }
        }

        // 再查L2
        auto it = l2_cache_.find(index);
        if (it != l2_cache_.end()) {
            entry = it->second;
            return true;
        }

        // 最后查L3
        return l3_storage_.get(index, entry);
    }

    void append(const LogEntry& entry) {
        // 添加到L1
        l1_cache_.push_back(entry);
        if (l1_cache_.size() > 1000) {
            l1_cache_.pop_front();
        }

        // 添加到L2
        l2_cache_[entry.index] = entry;
        if (l2_cache_.size() > 10000) {
            l2_cache_.erase(l1_cache_.front().index);
        }

        // 添加到L3
        l3_storage_.append(entry);
    }
};
```

**优化效果：**
- 减少磁盘访问：热点数据在内存中
- 降低延迟：L1缓存命中时延迟<1ms

### 9.4 选举优化

#### 9.4.1 优先投票给最新日志的候选人

```cpp
bool RaftNode::should_grant_vote(const RequestVoteMsg& msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 检查term
    if (msg.term < current_term_) {
        return false;
    }

    // 2. 检查是否已投票
    if (!voted_for_.is_null() && voted_for_ != msg.candidate_id) {
        return false;
    }

    // 3. [优化] 优先投票给最新日志的候选人
    uint32_t last_index, last_term;
    log_array_.get_last_info(last_index, last_term);

    if (msg.last_log_term > last_term ||
        (msg.last_log_term == last_term &&
         msg.last_log_index >= last_index)) {
        // 候选人的日志至少和自己一样新
        voted_for_ = msg.candidate_id;
        save_persistent_state();
        return true;
    }

    return false;
}
```

**优化效果：**
- 减少选举次数：优先选择日志完整的候选人
- 提升可用性：避免选举出日志落后的leader

---

## 10. 测试与验证

### 10.1 单元测试

```cpp
// 测试日志追加和截断
TEST(RaftLogArray, AppendAndTruncate) {
    RaftLogArray log_array("/tmp/raft_test");

    LogEntry entry1{1, 1, 0, "SET key1 value1"};
    LogEntry entry2{2, 1, 0, "SET key2 value2"};

    EXPECT_TRUE(log_array.append(entry1));
    EXPECT_TRUE(log_array.append(entry2));
    EXPECT_EQ(log_array.size(), 2);

    EXPECT_TRUE(log_array.truncate_from(2));
    EXPECT_EQ(log_array.size(), 1);
}

// 测试角色转换
TEST(RaftNode, RoleTransition) {
    RaftNode node("/tmp/raft_test", "127.0.0.1", 8888);

    EXPECT_EQ(node.get_role(), RaftRole::Follower);

    node.become_candidate();
    EXPECT_EQ(node.get_role(), RaftRole::Candidate);

    node.become_leader();
    EXPECT_EQ(node.get_role(), RaftRole::Leader);

    node.become_follower(node.current_term_ + 1);
    EXPECT_EQ(node.get_role(), RaftRole::Follower);
}
```

### 10.2 集成测试

```cpp
// 测试三节点集群选举
TEST(RaftCluster, Election) {
    // 启动3个节点
    RaftNode node1("/tmp/raft1", "127.0.0.1", 8001);
    RaftNode node2("/tmp/raft2", "127.0.0.1", 8002);
    RaftNode node3("/tmp/raft3", "127.0.0.1", 8003);

    // 等待选举完成
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 检查只有一个leader
    int leader_count = 0;
    if (node1.get_role() == RaftRole::Leader) leader_count++;
    if (node2.get_role() == RaftRole::Leader) leader_count++;
    if (node3.get_role() == RaftRole::Leader) leader_count++;

    EXPECT_EQ(leader_count, 1);
}

// 测试日志复制和提交
TEST(RaftCluster, LogReplication) {
    // ... 启动集群

    // 提交命令到leader
    RaftNode* leader = find_leader();
    Response response = leader->submit_command("SET key value");

    EXPECT_TRUE(response.success);

    // 等待日志复制
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 检查所有节点都有该日志
    EXPECT_TRUE(node1.has_log(response.log_index));
    EXPECT_TRUE(node2.has_log(response.log_index));
    EXPECT_TRUE(node3.has_log(response.log_index));
}
```

### 10.3 压力测试

```cpp
// 测试吞吐量
TEST(RaftPerformance, Throughput) {
    // ... 启动集群

    const int NUM_REQUESTS = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_REQUESTS; i++) {
        leader->submit_command("SET key" + std::to_string(i) + " value");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double qps = NUM_REQUESTS * 1000.0 / duration.count();
    LOG_INFO("QPS: %.2f", qps);

    EXPECT_GT(qps, 1000);  // 至少1000 QPS
}
```

---

## 11. 部署与运维

### 11.1 配置参数

| 参数名称 | 默认值 | 说明 | 推荐值 |
|---------|--------|------|--------|
| `election_timeout_min` | 150ms | 选举超时最小值 | 150-300ms |
| `election_timeout_max` | 300ms | 选举超时最大值 | 300-600ms |
| `heartbeat_interval` | 30ms | 心跳间隔 | 20-50ms |
| `max_batch_entries` | 1000 | 批量日志条目数 | 500-2000 |
| `sync_interval` | 100 | fsync间隔 (条) | 50-200 |
| `snapshot_interval` | 10000 | 快照间隔 (条) | 5000-50000 |

### 11.2 监控指标

```cpp
struct RaftMetrics {
    // 性能指标
    uint64_t qps;                          // 每秒请求数
    uint64_t latency_avg;                  // 平均延迟 (ms)
    uint64_t latency_p99;                  // P99延迟 (ms)

    // Raft指标
    uint32_t current_term;                 // 当前任期
    RaftRole role;                         // 当前角色
    uint32_t commit_index;                 // 已提交索引
    uint32_t last_applied;                 // 已应用索引

    // 日志指标
    uint32_t log_size;                     // 日志条目数
    uint32_t log_trailing;                 // 落后日志数

    // 网络指标
    uint32_t heartbeat_loss;               // 心跳丢失数
    uint32_t network_partition;           // 网络分区次数
};
```

### 11.3 故障排查

#### 11.3.1 Leader频繁选举

**症状：** 集群中leader频繁变更

**可能原因：**
1. 网络不稳定，心跳超时
2. 节点负载过高，处理延迟
3. 选举超时设置过短

**排查步骤：**
```bash
# 检查网络延迟
ping -i 0.001 <node_ip>

# 检查节点负载
top -p <pid>

# 调整选举超时
election_timeout_min = 300ms
election_timeout_max = 600ms
```

#### 11.3.2 日志复制延迟

**症状：** 提交命令后长时间未应用到状态机

**可能原因：**
1. follower节点磁盘I/O慢
2. 网络带宽不足
3. 批量复制未启用

**排查步骤：**
```bash
# 检查磁盘I/O
iostat -x 1

# 检查网络带宽
iftop

# 启用批量复制
max_batch_entries = 1000
```

---

## 12. 总结

### 12.1 核心优势

1. **线性一致性保证**
   - 两阶段提交确保日志按相同顺序应用
   - 强制覆盖机制解决日志冲突
   - 联合共识防止脑裂

2. **分区容错性**
   - 多数派机制保证网络分区时可用性
   - 随机超时避免活锁
   - 主动降级机制防止多leader

3. **运行效率优化**
   - 无上限日志数组 + 顺序写
   - 批量日志复制 + 并发发送
   - 分层日志缓存 + 快照机制

### 12.2 后续改进方向

1. **日志压缩**
   - 实现增量快照
   - 支持快照分片传输

2. **负载均衡**
   - 动态调整心跳间隔
   - 基于延迟优化路由

3. **跨数据中心部署**
   - 支持WAN环境优化
   - 实现异地多活

---

## 附录A: 术语表

| 术语 | 英文 | 说明 |
|-----|------|------|
| 任期 | Term | 逻辑时钟，单调递增 |
| 日志条目 | LogEntry | 包含命令和元数据的记录 |
| 提交索引 | Commit Index | 已提交的最高日志索引 |
| 应用索引 | Last Applied | 已应用到状态机的最高日志索引 |
| 心跳 | Heartbeat | Leader发送的空AppendEntries消息 |
| 多数派 | Majority | 超过半数节点 |
| 联合共识 | Joint Consensus | 两阶段配置变更机制 |

---

## 附录B: 参考文献

1. Diego Ongaro and John Ousterhout, "In Search of an Understandable Consensus Algorithm", 2014
2. Raft论文: https://raft.github.io/raft.pdf
3. Raft可视化: https://raft.github.io/

---

*本文档由TinyKV团队编写，版权所有 (C) 2026*
