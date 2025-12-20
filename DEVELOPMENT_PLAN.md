### **总体开发思路**

我们将采用自底向上、逐步迭代的方式构建整个系统。从核心的单机存储引擎开始，然后封装网络接口，最后扩展到分布式共识，确保每个阶段构建的功能都是稳定和可测试的。

### **阶段 0: 项目初始化与环境搭建**

**目标:** 创建项目骨架，配置好编译环境、依赖管理和单元测试框架。

**关键任务:**
1.  **创建目录结构:** 按照 `README.md` 中定义的结构，创建 `include`, `src`, `tests`, `cmake`, `lib` 等目录。
2.  **编写根 `CMakeLists.txt`:**
    *   定义项目名称 (`TinyKV`) 和 C++ 标准 (C++17)。
    *   配置头文件目录 (`include`) 和源文件目录 (`src`)。
    *   添加 `tests` 目录，并集成 GTest (Google Test) 框架。
    *   配置第三方库的引入路径（例如 `glog`, `protobuf`）。
3.  **编写各模块的 `CMakeLists.txt`:** 为 `src` 和 `tests` 目录创建基础的 `CMakeLists.txt` 文件，用于管理各自模块的源文件。
4.  **配置 `.gitignore`:** 添加构建产物 (`build/`, `bin/`) 和其他临时文件。
5.  **Hello World 测试:** 在 `src/main.cpp` 中编写一个简单的主函数，并在 `tests` 中编写一个基础的 GTest 测试用例，确保编译和测试流程可以正常工作。

**产出文件:**
*   完整的项目目录结构。
*   `CMakeLists.txt` (根目录及各子目录)。
*   `.gitignore` 文件。
*   一个可以成功编译并运行测试的基础项目。

**验证方法:**
*   在 `build` 目录下执行 `cmake ..` 和 `make` (或 `cmake --build .`) 能够成功编译。
*   执行 `ctest` 或直接运行测试二进制文件，可以看到 GTest 的测试用例通过。

---

### **阶段 1: 实现单机存储引擎 (Storage Engine)**

**目标:** 开发一个功能完备、支持持久化的单机 KV 存储引擎。

**关键任务:**
1.  **实现 MemTable:**
    *   **数据结构:** 使用 `std::map` 或实现一个更高效的跳表 (SkipList)。跳表为推荐选项。
    *   **接口:** 提供 `Put(key, value)`, `Get(key), Delete(key)` 方法。
    *   **位置:** `include/storage/memtable.h`, `src/storage/memtable.cpp`。
2.  **实现 WAL (Write-Ahead Log):**
    *   **功能:** 在数据写入 MemTable 之前，将操作（写/删）日志顺序追加到磁盘文件中。
    *   **恢复:** 程序启动时，检查并读取 WAL 文件，将未刷盘的数据恢复到 MemTable 中，保证崩溃不丢数据。
    *   **位置:** `include/storage/wal.h`, `src/storage/wal.cpp`。
3.  **实现 SSTable (Sorted String Table):**
    *   **功能:** 当 MemTable 大小达到预设阈值时，将其内容转换成一个有序的、不可变的 SSTable 文件并存入磁盘。
    *   **格式:** 设计 SSTable 的文件格式（例如，数据块、索引块、元数据块）。
    *   **位置:** `include/storage/sstable.h`, `src/storage/sstable.cpp`。
4.  **实现 Compaction (合并) - 简化版:**
    *   **功能:** 定期将多个小的 SSTable 文件合并成一个大的 SSTable 文件，以清除冗余数据（被覆盖或删除的键）。
    *   **策略:** 先实现最简单的 Leveled Compaction 或 Tiered Compaction 策略。
5.  **整合存储引擎:**
    *   创建一个 `Storage` 类，统一管理 MemTable, WAL 和 SSTable。
    *   `Put`/`Delete` 操作：写 WAL -> 写 MemTable。
    *   `Get` 操作：依次查询 MemTable、SSTable。

**产出文件:**
*   `memtable.h/.cpp`
*   `wal.h/.cpp`
*   `sstable.h/.cpp`
*   `storage.h/.cpp`
*   `tests/test_memtable.cpp`
*   `tests/test_storage.cpp`

**验证方法:**
*   **单元测试:** 针对 MemTable 的增删改查编写 GTest 测试。
*   **集成测试:** 编写 `test_storage` 测试完整的读写流程，包括：
    *   写入数据后能正确读出。
    *   模拟进程崩溃（写入部分数据后退出），重启后能通过 WAL 恢复数据。
    *   写入大量数据触发 MemTable 到 SSTable 的刷盘，验证数据能从 SSTable 中读出。

---

### **阶段 2: 实现网络模块与协议 (Network & Protocol)**

**目标:** 让存储引擎能通过网络接收和处理客户端请求。

**关键任务:**
1.  **定义协议格式 (Protobuf):**
    *   创建一个 `.proto` 文件 (例如 `kv_protocol.proto`)。
    *   定义请求和响应消息，如 `GetRequest`, `GetResponse`, `PutRequest`, `PutResponse`, `DeleteRequest`, `DeleteResponse`。
    *   在 CMake 中配置 `protobuf-compiler`，自动生成 C++ 代码。
2.  **实现 Reactor 网络模型:**
    *   **Epoll 封装:** 创建一个 Epoll 的 C++ 封装类，简化事件的添加、修改、删除。
    *   **Acceptor (主线程):** 监听服务器端口，接收新连接，并将其封装成一个 `Connection` 对象。
    *   **Worker 线程池:** 创建一组工作线程，每个线程运行一个独立的 Event Loop (事件循环)。Acceptor 将新连接轮询分配给 Worker 线程。
    *   **Connection 类:** 负责处理具体连接的读写事件、数据缓冲和协议解析。
    *   **位置:** `include/network/server.h`, `src/network/server.cpp`。
3.  **整合协议处理:**
    *   在 `Connection` 类的读事件回调中，从缓冲区读取数据，并尝试用 Protobuf 解析成请求消息。
    *   解析成功后，调用第一阶段完成的 `Storage` 模块的接口执行操作。
    *   将 `Storage` 模块返回的结果封装成 Protobuf 响应消息，序列化后发送给客户端。

**产出文件:**
*   `kv_protocol.proto` 及生成的 `.pb.h`, `.pb.cc`。
*   `include/network/server.h`, `src/network/server.cpp`。
*   `include/network/connection.h`, `src/network/connection.cpp`。
*   `src/main.cpp` 中启动网络服务器。
*   一个简单的测试客户端 `tests/test_client.cpp`。

**验证方法:**
*   启动 `TinyKV` 服务器。
*   运行测试客户端，可以成功连接服务器，并进行 `PUT`, `GET`, `DELETE` 操作，结果符合预期。
*   可以使用多个客户端并发请求，服务器能稳定处理。

---

### **阶段 3: 实现 Raft 核心逻辑 (Leader Election)**

**目标:** 迈出分布式的第一步，实现 Raft 算法的自动选举功能。

**关键任务:**
1.  **定义 Raft 节点角色:** 创建一个 `RaftNode` 类，并用枚举定义三种状态：`Follower`, `Candidate`, `Leader`。
2.  **实现 RPC 接口:**
    *   定义 Raft 节点间的通信消息 (Protobuf)，主要是 `RequestVoteRequest` / `Response` 和 `AppendEntriesRequest` / `Response`。
    *   `RaftNode` 之间通过第二阶段的网络模块进行 RPC 通信。
3.  **实现心跳与选举定时器:**
    *   **选举定时器 (Election Timer):** Follower 在一段时间内未收到 Leader 心跳，则超时触发选举，转为 Candidate。
    *   **心跳定时器 (Heartbeat Timer):** Leader 定期向所有 Follower 发送心跳（可以是轻量的 `AppendEntries` 请求）。
4.  **实现选举流程:**
    *   **Candidate:** 向所有其他节点发送 `RequestVote` 请求。
    *   **投票逻辑:** 节点收到 `RequestVote` 请求后，根据任期号 (Term) 和日志情况决定是否投票。
    *   **状态转换:** Candidate 收到超过半数节点的投票后，成为 Leader。如果选举超时或收到更高任期的 Leader 消息，则退回 Follower。

**产出文件:**
*   `include/raft/raft.h`, `src/raft/raft.cpp`。
*   更新 `.proto` 文件，加入 Raft RPC 消息定义。
*   `tests/test_raft_election.cpp`。

**验证方法:**
*   **多节点测试:** 在本地启动 3 或 5 个 `TinyKV` 实例，组成一个集群。
*   观察日志，验证：
    *   集群启动后能自动选举出一个 Leader。
    *   手动杀掉 Leader 进程后，剩余节点能发起新一轮选举并产生新 Leader。
    *   网络分区后，多数派分区能选举出 Leader，少数派分区不能。

---

### **阶段 4: 实现 Raft 数据同步 (Log Replication)**

**目标:** 实现 Raft 的核心功能——日志复制，保证集群数据的一致性。

**关键任务:**
1.  **实现日志模块:**
    *   在 `RaftNode` 中维护一个日志数组（`std::vector<LogEntry>`），`LogEntry` 包含指令和任期号。
2.  **实现日志复制流程:**
    *   客户端的写请求 (`PUT`/`DELETE`) 发送给 Leader。
    *   Leader 将指令封装成 `LogEntry`，追加到自己的日志中，然后通过 `AppendEntries` RPC 发送给所有 Follower。
    *   Follower 收到后，进行一致性检查，然后将日志追加到本地。
3.  **实现 Commit 机制:**
    *   Leader 收到超过半数 Follower 的成功响应后，将该日志条目标记为 "committed"。
    *   Leader 将 `commitIndex` （已提交日志的最高索引）通过后续的 `AppendEntries` RPC 通知给所有 Follower。
    *   节点（包括 Leader 和 Follower）将 "committed" 的日志应用到第一阶段实现的 **状态机** (即 `Storage` 引擎) 中。
4.  **修改服务器逻辑:**
    *   服务器接收到写请求后，不再直接调用 `Storage` 模块，而是将其提交给 `RaftNode`。
    *   `RaftNode` 提交成功后，再通过回调机制将结果返回给客户端。

**产出文件:**
*   重度修改 `raft.h/.cpp`。
*   修改 `network/server.cpp` 中处理客户端请求的逻辑。
*   `tests/test_raft_replication.cpp`。

**验证方法:**
*   **多节点一致性测试:**
    *   向 Leader 发送一个 `PUT` 请求。
    *   稍后连接到任意一个 Follower，执行 `GET` 请求，应能读到刚刚写入的数据。
    *   测试当有 Follower 宕机时，数据依然能被复制到大多数节点并提交。
    *   宕机的 Follower 重启后，能够通过 Raft 的日志恢复机制自动追赶上最新的数据。

---

### **阶段 5: 优化与完善**

**目标:** 提升系统性能、健壮性和可观测性。

**关键任务:**
1.  **存储引擎优化:**
    *   **Bloom Filter:** 在 SSTable 中加入布隆过滤器，快速判断某个 key **不存在**，避免不必要的磁盘 I/O。
    *   **并发优化:** 使用读写锁 (`std::shared_mutex`) 优化 `Storage` 模块，允许多个读操作并发执行。
2.  **Raft 优化:**
    *   **日志快照 (Snapshot):** 当日志变得非常大时，定期为状态机创建一个快照，并清理旧日志，加快节点重启和新节点加入的速度。
    *   **批量提交:** Leader 可以一次性发送多个日志条目，Follower 也可以批量确认，减少 RPC 次数。
3.  **增加监控和日志:**
    *   使用 `glog` 或 `spdlog` 记录关键路径的日志。
    *   暴露系统的关键指标（如 QPS、延迟、Raft 状态等）。

**产出:**
*   一个性能更高、更鲁棒的分布式 KV 存储系统。

**验证方法:**
*   **基准测试 (Benchmark):** 编写压测工具，测试系统的吞吐量 (QPS) 和延迟 (Latency)。
*   **故障注入测试:** 模拟各种异常情况（节点宕机、网络延迟、丢包），验证系统的自愈能力。

---

此开发方案为您提供了一个从零到一的清晰路径。建议严格按照阶段进行，完成一个阶段的充分测试后再进入下一阶段，这将大大提高开发效率和项目成功率。