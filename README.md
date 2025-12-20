构建一个轻量级分布式 KV 存储引擎是一个极佳的 C++ 实战项目。它涵盖了**存储引擎设计、网络编程、分布式共识算法、多线程并发**等核心后端技术。

以下是该项目的完整路线图。

---

### 一、 项目介绍 (Project Overview)

**项目名称：** TinyKV
**项目描述：** 这是一个基于 C++ 开发的、高性能、持久化的分布式键值存储系统。它结合了 LSM-Tree 存储模型以保证高吞吐写入，并利用 Raft 算法实现多节点之间的数据一致性，对外提供类似 Redis 的网络接口。

**核心功能：**
1.  **基础 KV 操作：** 支持 `GET`、`PUT`、`DELETE` 等操作。
2.  **持久化：** 基于 LSM-Tree 结构，支持数据落盘，防止宕机丢失。
3.  **高可用（分布式）：** 通过 Raft 算法实现 Leader 选举和日志复制，支持多副本冗余。
4.  **高性能网络：** 基于 Reactor 模型的非阻塞 IO 处理并发请求。
5.  **简单协议：** 采用protobuf。

---

### 二、 项目骨架 (Project Skeleton)

使用 CMake 构建，目录结构建议如下：

```text
TinyKV/
├── bin/                # 编译生成的二进制文件
├── build/              # 构建目录
├── cmake/              # CMake 脚本配置
├── include/            # 头文件 (.h, .hpp)
│   ├── common/         # 公共宏、工具类
│   ├── storage/        # 存储引擎模块
│   ├── network/        # 网络模块
│   ├── raft/           # Raft 共识模块
│   └── protocol/       # 协议解析模块
├── src/                # 源文件 (.cpp)
│   ├── storage/
│   ├── network/
│   ├── raft/
│   └── main.cpp        # 程序入口
├── tests/              # 单元测试 (GTest)
├── lib/        # 第三方库 (如 gflags, glog, nlohmann_json)
├── CMakeLists.txt      # 根 CMake 配置文件
└── README.md
```

---

### 三、 核心模块实现方案

#### 1. 存储引擎模块 (Storage Engine)
*   **技术选择：** **LSM-Tree (Log-Structured Merge-Tree)**。相比 B+ 树，LSM 在写密集型场景表现更优。
*   **关键组件：**
    *   **MemTable:** 内存中的跳表 (SkipList)。新数据先写跳表，保证 $O(\log N)$ 的读写效率。
    *   **WAL (Write Ahead Log):** 写前日志。数据写入 MemTable 前先顺序写入 WAL，用于崩溃恢复。
    *   **SSTable (Sorted String Table):** 当 MemTable 达到阈值时，将其冻结并异步刷入磁盘，形成有序文件。
    *   **Compaction (合并):** 定期将多个 SSTable 合并，删除旧版本或已删除的数据，减少空间放大。
*   **数据结构：** `std::map` (简单实现) 或 **SkipList** (推荐，更接近工业级)。

#### 2. 网络模块 (Network Module)
*   **技术选择：** **Epoll (Linux) + Reactor 模式**。
*   **实现细节：**
    *   使用 `epoll` 实现多路复用。
    *   **主线程 (Acceptor):** 负责监听端口，接收新连接，并分发给 Worker 线程。
    *   **工作线程池 (Worker Pool):** 每个 Worker 维护一个事件循环 (Event Loop)，负责读取数据、解析协议并执行逻辑。
    *   **非阻塞 IO:** 确保在高并发下不会因为某个 Socket 的读写而阻塞整个进程。

#### 3. 协议模块 (Protocol Module)
*   **技术选择：** **protobuf**。

#### 4. 分布式共识模块 (Raft Module) - *最难点*
*   **技术选择：** **Raft 算法**。
*   **核心功能：**
    *   **Leader Election:** 处理节点宕机后的自动选主。
    *   **Log Replication:** Leader 将客户端的 PUT 请求包装成日志条目，同步给 Follower。
    *   **Commit Index:** 只有过半数节点确认后，日志才应用到存储引擎。
*   **实现技巧：**
    *   定义三种状态：Follower, Candidate, Leader。
    *   定时器 (Timer)：用于心跳检测和选举超时。
    *   RPC 通信：节点间通过网络模块发送 `AppendEntries` 和 `RequestVote` 请求。
---

### 四、 关键技术栈推荐

1.  **语言标准：** C++17 或 C++20 (利用 `std::variant`, `std::optional`, `std::jthread`)。
2.  **构建系统：** CMake 3.15+。
3.  **日志：** `glog` 或 `spdlog`。
4.  **测试：** `Google Test (GTest)`。
5.  **并发：** `std::thread`, `std::mutex`, `std::condition_variable`。如果想挑战高难度，可以考虑 **无锁队列 (Lock-free Queue)**。
6.  **序列化：** `Protobuf` (推荐，跨语言且高效)。

---

### 五、 开发阶段划分 (Roadmap)

*   **Phase 1: 单机版存储。** 先写出一个基于内存跳表和 WAL 的简单 KV 存储，支持持久化。
*   **Phase 2: 网络交互。** 加入 Epoll 封装，让客户端可以通过 `telnet` 或简单 Client 发送指令。
*   **Phase 3: 协议规范。** 实现完整的协议解析，处理并发请求。
*   **Phase 4: Raft 集成。** 实现选主逻辑。这是从单机走向分布式的跨越。
*   **Phase 5: 数据同步。** 实现日志复制和一致性保证。
*   **Phase 6: 优化。** 加入 Bloom Filter (减少 SSTable 磁盘 IO)、读写锁优化、多线程 Compaction。

### 六、 为什么这么设计？

*   **跳表 vs 红黑树：** 跳表实现简单，且方便实现高并发版本（通过 CAS 操作可以做到无锁化）。
*   **LSM-Tree vs B+树：** 现代分布式存储（如 TiDB, RocksDB）多采用 LSM，因为它将随机写转化为顺序写，极大提升了磁盘性能。
*   **Raft vs Paxos：** Raft 逻辑更清晰，易于工程实现和调试。