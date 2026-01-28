# Raft 快速开始指南

## 概述

本指南帮助您快速开始使用 TinyKV 的 Raft 分布式共识算法实现。

## 编译说明

### 前置要求
- CMake 3.10+
- C++17 编译器
- 系统库：`pthread`, `socket`, `fsync`

### 编译步骤

```bash
cd e:\TinyKV
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## 快速开始

### 1. 启动首个节点（自举为Leader）

```bash
# 在第一个终端
./build/tinykv --raft --port 8001 --root /data/node1 --password "mypassword"
```

节点将自举为 Leader，等待其他节点加入。

### 2. 启动后续节点（作为Follower）

```bash
# 在第二个终端
./build/tinykv --raft --port 8002 --root /data/node2 --password "mypassword"

# 在第三个终端
./build/tinykv --raft --port 8003 --root /data/node3 --password "mypassword"
```

节点会尝试连接已知的 leader，并同步日志。

### 3. 验证集群状态

```bash
# 查询leader
echo "get_master" | nc 127.0.0.1 8001

# 查看节点状态
echo "get_status" | nc 127.0.0.1 8001

# 列出集群节点
echo "list_nodes" | nc 127.0.0.1 8001
```

### 4. 提交命令（两阶段提交）

```bash
# 通过leader提交命令
echo "SET key1 value1" | nc 127.0.0.1 8001
echo "SET key2 value2" | nc 127.0.0.1 8001
echo "GET key1" | nc 127.0.0.1 8001
```

## 集群管理

### 添加节点

```bash
# 通过leader添加新节点
echo "add_node 127.0.0.1:8004" | nc 127.0.0.1 8001
```

### 移除节点

```bash
# 通过leader移除节点
echo "remove_node 127.0.0.1:8004" | nc 127.0.0.1 8001
```

## 数据目录结构

```
/data/node1/
├── .raft/
│   ├── raft_log.bin          # 日志文件（顺序写）
│   └── .raft_meta            # 持久化状态
└── storage.db                # KV存储（状态机）
```

## 核心特性

### ✅ 已实现
- [x] 无上限日志数组（支持无限增长）
- [x] 顺序写持久化
- [x] 三场景初始化（自举、follower、探查）
- [x] 角色转换（Follower ⇄ Candidate ⇄ Leader）
- [x] 选举机制（随机超时、多数派投票）
- [x] 日志复制（AppendEntries、心跳）
- [x] 两阶段提交（预写日志）
- [x] 强制覆盖（解决日志冲突）
- [x] 集群管理（添加/移除节点）

### ⏳ 待完善
- [ ] 完整网络消息发送/接收
- [ ] 快照机制
- [ ] 联合共识（配置变更）
- [ ] 日志清理和压缩
- [ ] 性能优化（批量复制、并发）

## 配置参数

在 `include/raft/raft.h` 中可配置以下参数：

```cpp
// 选举超时（毫秒）
int election_timeout_min_ = 150;  // 最小值
int election_timeout_max_ = 300;  // 最大值

// 心跳间隔（毫秒）
int heartbeat_interval_ = 30;

// 日志配置（在 protocol.h 中）
#define RAFT_LOG_BUFFER_SIZE 8192  // 日志缓冲区大小（已弃用）
```

## 监控和调试

### 查看日志

```bash
# 日志输出到控制台
./tinykv --raft --port 8001 --root /data/node1 2>&1 | tee raft.log
```

### 性能指标

使用以下命令监控 Raft 性能：

```bash
# 查看节点状态
echo "get_status" | nc 127.0.0.1 8001

# 输出示例：
# role=1,term=5,leader=127.0.0.1:8001,nodes=3,commit=100,applied=98
# 解释：
#   role=1    - 0=Follower, 1=Candidate, 2=Leader
#   term=5     - 当前任期
#   leader     - leader地址
#   nodes=3    - 集群节点数
#   commit=100 - 已提交最高日志索引
#   applied=98  - 已应用到状态机最高索引
```

## 常见问题

### Q1: 如何判断节点是否为leader？

```bash
# 方法1: 使用get_status命令
echo "get_status" | nc <node_ip> <port>
# role=2 表示leader

# 方法2: 使用get_master命令
echo "get_master" | nc <node_ip> <port>
# 如果返回该节点地址，则为leader
```

### Q2: 如何处理leader故障？

Raft 会自动选举新的 leader：

1. follower 检测到 leader 超时（>150ms）
2. 转换为 candidate 并发起选举
3. 获得多数选票的节点成为新 leader
4. 所有节点自动同步到新 leader

无需手动干预！

### Q3: 如何查看日志？

```bash
# Raft日志存储在数据目录
cat /data/node1/.raft/raft_log.bin | strings
```

### Q4: 如何重置集群？

删除所有节点的 `.raft` 目录并重启：

```bash
rm -rf /data/node1/.raft
rm -rf /data/node2/.raft
rm -rf /data/node3/.raft
```

然后重新启动节点，首个节点将自举为 leader。

## 性能测试

### 测试吞吐量

```bash
# 使用简单的shell脚本测试
for i in {1..1000}; do
    echo "SET key$i value$i" | nc 127.0.0.1 8001
done
```

### 测试延迟

```bash
# 使用time命令测量
time echo "GET key1" | nc 127.0.0.1 8001
```

## 故障测试

### 模拟网络分区

```bash
# 阻止节点间的通信（Linux）
sudo iptables -A INPUT -s <other_node_ip> -j DROP
sudo iptables -A OUTPUT -d <other_node_ip> -j DROP

# 恢复通信
sudo iptables -D INPUT -s <other_node_ip> -j DROP
sudo iptables -D OUTPUT -d <other_node_ip> -j DROP
```

### 模拟节点故障

```bash
# 直接kill进程
killall tinykv
```

## 下一步

1. 查看 `RAFT_IMPLEMENTATION.md` 了解实现细节
2. 查看 `RAFT_DESIGN.md` 了解设计文档
3. 查看单元测试示例
4. 根据实际需求调整配置参数

## 获取帮助

- 查看设计文档：`RAFT_DESIGN.md`
- 查看实现进度：`RAFT_IMPLEMENTATION.md`
- 查看API文档：（待添加）

---

*最后更新: 2026-01-28*
