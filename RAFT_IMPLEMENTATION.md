# Raft 实现进度说明

## 实现状态

### ✅ 已完成的功能

#### 1. 协议层更新 (`include/raft/protocol/protocol.h`)
- ✅ 扩展 `LogEntry` 结构，添加 `command_type` 和 `timestamp` 字段
- ✅ 新增消息类型枚举：
  - `QUERY_LEADER` / `QUERY_LEADER_RESPONSE` - 探查leader
  - `JOIN_CLUSTER` / `JOIN_CLUSTER_RESPONSE` - 新节点加入
  - `CLUSTER_CONFIG` / `CLUSTER_CONFIG_RESPONSE` - 集群配置变更
  - `SNAPSHOT_INSTALL` / `SNAPSHOT_INSTALL_RESPONSE` - 快照安装
- ✅ 实现新的消息数据结构：
  - `ClusterMetadata` - 集群元数据
  - `QueryLeaderData` / `QueryLeaderResponseData`
  - `JoinClusterData` / `JoinClusterResponseData`
  - `ClusterConfigData` / `ClusterConfigResponseData`
  - `SnapshotInstallData` / `SnapshotInstallResponseData`
- ✅ 更新 `RaftMessage` 结构，支持所有新消息类型

#### 2. 数据结构更新 (`include/raft/raft.h`)
- ✅ 添加 `PersistentState` 结构（持久化状态）
- ✅ 创建 `RaftLogArray` 类（替换 `RaftLogRingBuffer`）
  - 支持无上限日志数组
  - 顺序写持久化
  - 批量追加和截断
- ✅ 更新 `RaftNode` 类成员变量：
  - 使用 `string` 类型的地址（便于序列化）
  - 添加 `cluster_metadata_` 和 `persistent_state_`
  - 添加日志目录路径

#### 3. 核心功能实现 (`src/raft/raft.cpp`)

##### 3.1 初始化流程
- ✅ 实现完整的初始化构造函数
- ✅ 场景1: `init_as_follower()` - 作为follower启动
- ✅ 场景2: `init_as_bootstrap_leader()` - 自举为leader
- ✅ 场景3: `init_with_cluster_discovery()` - 探查集群
- ✅ `load_persistent_state()` - 加载持久化状态
- ✅ `save_persistent_state()` - 保存持久化状态
- ✅ `start_background_threads()` - 启动后台线程

##### 3.2 角色转换
- ✅ `become_follower()` - 转换为follower
- ✅ `become_candidate()` - 转换为candidate
- ✅ `become_leader()` - 转换为leader

##### 3.3 选举机制
- ✅ `election_loop()` - 选举线程主循环
- ✅ `send_request_vote()` - 发送投票请求
- ✅ `handle_request_vote()` - 处理投票请求（包含日志完整性检查）
- ✅ `handle_request_vote_response()` - 处理投票响应

##### 3.4 日志复制
- ✅ `heartbeat_loop()` - 心跳线程主循环
- ✅ `send_heartbeat_to_all()` - 发送心跳到所有节点
- ✅ `send_append_entries()` - 发送AppendEntries（支持批量）
- ✅ `handle_append_entries()` - 处理AppendEntries请求
- ✅ `handle_append_entries_response()` - 处理AppendEntries响应
- ✅ `try_commit_entries()` - 尝试提交日志（多数派判定）
- ✅ `commit_entry()` - 提交指定索引的日志
- ✅ `apply_committed_entries()` - 应用已提交日志到状态机

##### 3.5 两阶段提交
- ✅ `submit_command()` - 实现预写日志（阶段1）
- ⚠️  日志复制提交（阶段2）- 部分实现，需完善网络发送

##### 3.6 集群管理
- ✅ `handle_join_cluster()` - 处理新节点加入
- ✅ `handle_query_leader()` - 处理探查leader
- ✅ `add_new_connection()` - 新连接处理（包含leader检测）
- ✅ `sync_cluster_metadata()` - 同步集群配置（框架实现）
- ✅ `sync_missing_logs()` - 同步缺失日志（框架实现）

##### 3.7 Raft管理命令
- ✅ 扩展 `handle_raft_command()` 支持新命令：
  - `add_node` - 添加节点
  - `remove_node` - 移除节点
  - `list_nodes` - 列出所有节点
  - `get_status` - 获取节点状态

##### 3.8 异常处理
- ✅ `on_active_connect()` - 主动连接降级机制
- ✅ `count_reachable_nodes()` - 计算可达节点数（框架实现）
- ✅ `stop_accepting_writes()` - 停止接受写请求（框架实现）

#### 4. 日志存储 (`src/raft/raft_log_array.cpp`)
- ✅ 实现 `RaftLogArray` 类：
  - `append()` - 单条日志追加（内存+磁盘）
  - `batch_append()` - 批量日志追加
  - `get()` - 获取指定索引日志
  - `truncate_from()` - 截断日志
  - `get_last()` - 获取最后一条日志
  - `get_last_info()` - 获取最后一条日志的索引和任期
  - `get_term()` - 获取指定索引的日志任期
  - `get_entries_from()` - 获取从指定索引开始的所有日志
  - `load_from_disk()` - 从磁盘加载日志
  - `flush()` - 刷盘

---

### ⏳ 需要完善的功能（TODO）

#### 高优先级
1. **网络消息发送** - 目前只实现了消息处理，缺少实际的网络发送
   - 在 `send_append_entries()` 中添加 `TCPClient` 发送逻辑
   - 在 `send_request_vote()` 中添加 `TCPClient` 发送逻辑
   - 实现消息接收和分发机制

2. **完整的两阶段提交** - 目前只实现了预写日志
   - 在 `submit_command()` 后立即触发日志复制
   - 实现投票统计和提交机制
   - 实现客户端结果查询

3. **消息分发器** - 实现 `handle_request()` 的完整逻辑
   - 根据消息类型分发到不同的处理函数
   - 处理 `AppendEntriesResponse` 和 `RequestVoteResponse`

#### 中优先级
4. **快照机制**
   - 实现 `create_snapshot()` 方法
   - 实现快照压缩和传输
   - 实现快照恢复

5. **联合共识** - 集群配置变更
   - 完善 `ClusterConfigData` 的处理
   - 实现两阶段配置变更流程
   - 处理多数派验证

6. **日志清理**
   - 定期清理已提交的旧日志
   - 实现快照索引机制
   - 优化磁盘空间使用

#### 低优先级
7. **监控和统计**
   - 实现性能指标收集
   - 实现心跳丢失检测
   - 实现网络分区检测

8. **性能优化**
   - 实现分层日志缓存（L1/L2/L3）
   - 实现批量复制优化
   - 实现并发复制

---

## 使用示例

### 1. 初始化Raft节点

```cpp
// 作为首个节点启动（自举为leader）
RaftNode::init("/data/raft", "127.0.0.1", 8001, 3, "password");

// 作为follower节点启动（连接到已知leader）
// 需要在持久化状态中配置leader_address
RaftNode::init("/data/raft", "127.0.0.1", 8002, 3, "password");
```

### 2. 提交命令（两阶段提交）

```cpp
// 获取leader
RaftNode* node = RaftNode::get_instance();
if (node->get_role() == RaftRole::Leader) {
    // 提交命令（预写日志）
    Response response = node->submit_command("SET key value");
    if (response.success) {
        uint32_t log_index = std::stoi(response.data);
        // 客户端可以使用log_index查询结果
    }
}
```

### 3. 集群管理命令

```bash
# 添加节点到集群（仅leader）
add_node 127.0.0.1:8003

# 移除节点（仅leader）
remove_node 127.0.0.1:8003

# 列出所有节点
list_nodes

# 获取节点状态
get_status

# 查询leader
get_master
```

---

## 架构特性

### ✅ 已实现的特性

1. **线性一致性保证**
   - ✅ 两阶段提交设计（预写+提交）
   - ✅ 强制覆盖机制解决日志冲突
   - ✅ 日志完整性检查（RequestVote）

2. **分区容错性**
   - ✅ 多数派机制（`try_commit_entries()`）
   - ✅ 随机超时避免活锁（`generate_election_timeout()`）
   - ✅ 主动降级机制（`become_follower()`）

3. **运行效率优化**
   - ✅ 无上限日志数组（替换环形缓冲区）
   - ✅ 顺序写持久化
   - ✅ 批量日志追加（`batch_append()`）

### ⏳ 待实现的特性

1. **联合共识** - 防止脑裂
2. **快照机制** - 加速恢复和节省空间
3. **性能优化** - 分层缓存和并发复制

---

## 测试建议

### 单元测试
```cpp
// 测试日志追加和截断
TEST(RaftLogArray, AppendAndTruncate) {
    RaftLogArray log_array("/tmp/raft_test");

    LogEntry entry1(1, 1, "SET key1 value1", 0, 0);
    LogEntry entry2(2, 2, "SET key2 value2", 0, 0);

    EXPECT_TRUE(log_array.append(entry1));
    EXPECT_TRUE(log_array.append(entry2));
    EXPECT_EQ(log_array.size(), 2);

    EXPECT_TRUE(log_array.truncate_from(2));
    EXPECT_EQ(log_array.size(), 1);
}

// 测试角色转换
TEST(RaftNode, RoleTransition) {
    // 需要先初始化RaftNode
    RaftNode* node = RaftNode::get_instance();

    EXPECT_EQ(node->get_role(), RaftRole::Follower);

    node->become_candidate();
    EXPECT_EQ(node->get_role(), RaftRole::Candidate);

    node->become_leader();
    EXPECT_EQ(node->get_role(), RaftRole::Leader);

    node->become_follower(node->get_current_term() + 1);
    EXPECT_EQ(node->get_role(), RaftRole::Follower);
}
```

### 集成测试
```bash
# 启动3个节点
./tinykv --raft --port 8001 --root /data/node1
./tinykv --raft --port 8002 --root /data/node2
./tinykv --raft --port 8003 --root /data/node3

# 通过命令行测试
echo "get_master" | nc 127.0.0.1 8001
echo "add_node 127.0.0.1:8002" | nc 127.0.0.1 8001
echo "SET key value" | nc 127.0.0.1 8001
```

---

## 性能优化建议

### 1. 存储优化
- 使用 `O_DIRECT` 标志绕过页缓存
- 定期批量 `fsync` 而非每次 `fsync`
- 实现快照压缩

### 2. 网络优化
- 实现批量日志复制（最多1000条）
- 使用线程池并发发送
- 实现连接池复用

### 3. 内存优化
- 实现三层缓存（L1: 1000条，L2: 10000条，L3: 磁盘）
- 使用内存池减少分配开销
- 实现零拷贝序列化

---

## 故障排查

### 1. Leader频繁选举
**症状：** 集群中leader频繁变更

**排查：**
```bash
# 检查网络延迟
ping -i 0.001 <node_ip>

# 检查节点负载
top -p <pid>

# 调整选举超时（修改代码中的配置）
election_timeout_min_ = 300;  # 150ms -> 300ms
election_timeout_max_ = 600;  # 300ms -> 600ms
```

### 2. 日志复制延迟
**症状：** 提交命令后长时间未应用到状态机

**排查：**
```bash
# 检查磁盘I/O
iostat -x 1

# 检查网络带宽
iftop

# 查看日志
tail -f /data/raft/.raft/raft.log
```

---

## 下一步计划

1. **完善网络通信** - 实现完整的消息发送和接收
2. **实现快照机制** - 定期创建快照并清理旧日志
3. **添加单元测试** - 覆盖核心功能
4. **性能测试** - 测试吞吐量和延迟
5. **完善文档** - 添加API文档和用户手册

---

## 参考资源

- Raft论文: https://raft.github.io/raft.pdf
- Raft可视化: https://raft.github.io/
- 设计文档: `RAFT_DESIGN.md`

---

*本文档由TinyKV团队编写，最后更新: 2026-01-28*
