# EyaKV - 分布式键值存储系统

[![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)](https://github.com)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.15+-blue.svg)](https://cmake.org/)

## 目录

- [项目概述](#项目概述)
  - [核心功能](#核心功能)
  - [技术栈](#技术栈)
  - [应用场景](#应用场景)
- [完整的运行指南](#完整的运行指南)
  - [环境要求](#环境要求)
  - [依赖安装](#依赖安装)
  - [编译项目](#编译项目)
  - [配置说明](#配置说明)
  - [启动方法](#启动方法)
- [代码结构说明](#代码结构说明)
  - [目录结构](#目录结构)
  - [核心模块解析](#核心模块解析)

---

## 项目概述

EyaKV 是一个基于 C++17 开发的、高性能、持久化的分布式键值存储系统。它结合了 **LSM-Tree** 存储模型以保证高吞吐写入，并利用 **Raft** 算法实现多节点之间的数据一致性，对外提供类似 Redis 的网络接口。

### 核心功能

1. **基础 KV 操作**：支持 `GET`、`PUT`、`DELETE` 等操作
2. **复杂数据类型**：支持 String、List、Set、Hash、ZSet 等数据结构
3. **持久化存储**：基于 LSM-Tree 结构，支持数据落盘，防止宕机丢失
4. **高可用（分布式）**：通过 Raft 算法实现 Leader 选举和日志复制，支持多副本冗余
5. **高性能网络**：基于 Reactor 模型的非阻塞 IO 处理并发请求，支持跨平台（Linux/macOS/Windows）
6. **灵活配置**：支持通过配置文件和环境变量进行系统配置
7. **快照机制**：支持创建和恢复数据快照，便于数据备份和迁移
8. **日志管理**：支持日志轮转和级别控制（DEBUG/INFO/WARN/ERROR/FATAL）

### 技术栈

| 模块 | 技术 | 说明 |
|------|------|------|
| 编程语言 | C++17 | 现代 C++ 特性（std::variant、std::optional 等） |
| 构建系统 | CMake 3.15+ | 跨平台构建支持 |
| 日志库 | glog 0.6.0 | Google 开源日志库 |
| 测试框架 | Google Test 1.14.0 | 单元测试支持 |
| 存储模型 | LSM-Tree | MemTable（跳表）+ WAL + SSTable |
| 共识算法 | Raft | Leader 选举、日志复制、一致性保证 |
| 网络模型 | IO 复用 | Linux epoll / macOS kqueue / Windows select |
| 并发模型 | 线程池 + 条件变量 | 生产者-消费者模式 |
| 序列化协议 | 自定义协议 | 类 protobuf 的高效二进制协议 |

### 应用场景

- **分布式缓存系统**：缓存键值对数据
- **配置中心**：支持强一致性的配置存储
- **消息队列存储后端**：可靠的消息持久化
- **实时数据存储**：高并发的实时数据读写
- **学习研究**：分布式系统和存储引擎的学习项目

---

## 完整的运行指南

### 环境要求

#### 操作系统
- **Linux**：Ubuntu 18.04+、CentOS 7+、Debian 9+
- **macOS**：10.14+
- **Windows**：Windows 10+（推荐使用 Visual Studio 2019 或更高版本）

#### 编译器
- **GCC**：8.0 或更高版本
- **Clang**：10.0 或更高版本
- **MSVC**：Visual Studio 2019 (MSVC 19.14) 或更高版本

#### 必需工具
- **CMake**：3.15 或更高版本
- **Git**：用于克隆代码库
- **Make**（Linux/macOS）：构建工具
- **Ninja**（可选）：更快的构建工具

### 依赖安装

项目使用 CMake 的 FetchContent 自动下载依赖，无需手动安装 glog 和 gtest。

#### Linux（Ubuntu/Debian）

```bash
# 安装编译工具
sudo apt-get update
sudo apt-get install -y build-essential cmake git

# 可选：安装 Ninja（更快的构建）
sudo apt-get install -y ninja-build
```

#### macOS

```bash
# 使用 Homebrew 安装
brew install cmake git

# 可选：安装 Ninja
brew install ninja
```

#### Windows

```cmd
:: 安装 Visual Studio 2019 或更高版本（包含 CMake）
:: 或使用 Chocolatey
choco install cmake git

:: 如果使用 MinGW
choco install mingw
```

### 编译项目

#### Linux / macOS

```bash
# 1. 克隆代码库
git clone https://github.com/Elysia125/TinyKV.git
cd EyaKV

# 2. 创建构建目录
mkdir -p build
cd build

# 3. 配置 CMake（使用 Ninja 可加速）
cmake .. -G Ninja

# 4. 编译项目
ninja

# 或使用 Unix Makefiles
# cmake ..
# make -j$(nproc)
```

#### Windows（使用 Visual Studio）

```cmd
:: 1. 克隆代码库
git clone https://github.com/Elysia125/TinyKV.git
cd EyaKV

:: 2. 打开 "Developer Command Prompt for VS 2019"

:: 3. 创建构建目录
mkdir build
cd build

:: 4. 配置 CMake
cmake .. -G "Visual Studio 16 2019" -A x64

:: 5. 编译项目
cmake --build . --config Release
```

#### Windows（使用 MinGW）

```cmd
:: 1. 克隆代码库
git clone https://github.com/your-username/EyaKV.git
cd EyaKV

:: 2. 创建构建目录
mkdir build
cd build

:: 3. 配置 CMake
cmake .. -G "MinGW Makefiles"

:: 4. 编译项目
mingw32-make -j
```

编译成功后，可执行文件位于：`build/bin/`
### 配置说明

EyaKV 支持通过配置文件或环境变量进行配置。

#### 配置文件

默认配置文件路径：`build/conf/eyakv.conf`

配置文件格式（键值对）：
```
# 网络配置
port=5210
ip=0.0.0.0
raft_port=5211
raft_trust_ip=127.0.0.1

# 日志配置
log_level=1           # 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=FATAL
log_rotate_size=5120

# 存储配置
memtable_size=1073741824      # MemTable 大小（字节）
wal_enable=true
wal_file_size=1073741824
wal_file_max_count=10
wal_flush_interval=1000
wal_flush_strategy=0          # 0=后台线程, 1=立即刷新, 2=OS 缓冲

# Raft 配置
raft_election_timeout_min_ms=150
raft_election_timeout_max_ms=300
raft_heartbeat_interval_ms=30
raft_rpc_timeout_ms=2000
raft_append_entries_max_batch=100
raft_threadpool_workers=4
```

#### 环境变量配置

EyaKV 支持通过环境变量直接配置各个参数，配置优先级：**环境变量 > 配置文件 > 默认值**。

**环境变量命名规则**：`EYAKV_<CONFIG_KEY>`（配置 key 转为大写）

**特殊环境变量**：
- `EYAKV_CONFIG_PATH`：指定配置文件路径

**示例配置**：

```bash
# Linux / macOS
export EYAKV_PORT=5210
export EYAKV_LOG_LEVEL=0              # DEBUG 级别
export EYAKV_MEMTABLE_SIZE=2147483648 # 2GB
export EYAKV_RAFT_ELECTION_TIMEOUT_MIN_MS=200
export EYAKV_RAFT_HEARTBEAT_INTERVAL_MS=50

# 指定自定义配置文件路径
export EYAKV_CONFIG_PATH=/path/to/custom/config.conf

# Windows
set EYAKV_PORT=5210
set EYAKV_LOG_LEVEL=0
set EYAKV_MEMTABLE_SIZE=2147483648
set EYAKV_CONFIG_PATH=C:\path\to\custom\config.conf
```

**支持的环境变量**（完整列表）：

网络配置：
- `EYAKV_PORT`、`EYAKV_IP`、`EYAKV_RAFT_PORT`、`EYAKV_RAFT_TRUST_IP`

日志配置：
- `EYAKV_LOG_LEVEL`、`EYAKV_LOG_ROTATE_SIZE`、`EYAKV_LOG_DIR`

存储配置：
- `EYAKV_MEMTABLE_SIZE`、`EYAKV_DATA_DIR`、`EYAKV_READ_ONLY`

SkipList 配置：
- `EYAKV_SKIPLIST_MAX_LEVEL`、`EYAKV_SKIPLIST_PROBABILITY`、`EYAKV_SKIPLIST_MAX_NODE_COUNT`

WAL 配置：
- `EYAKV_WAL_ENABLE`、`EYAKV_WAL_DIR`、`EYAKV_WAL_FILE_SIZE`、`EYAKV_WAL_FILE_MAX_COUNT`、`EYAKV_WAL_FLUSH_INTERVAL`、`EYAKV_WAL_FLUSH_STRATEGY`

SSTable 配置：
- `EYAKV_SSTABLE_MERGE_STRATEGY`、`EYAKV_SSTABLE_ZERO_LEVEL_SIZE`、`EYAKV_SSTABLE_LEVEL_SIZE_RATIO`、`EYAKV_SSTABLE_MERGE_THRESHOLD`

连接和线程配置：
- `EYAKV_MAX_CONNECTIONS`、`EYAKV_MEMORY_POOL_SIZE`、`EYAKV_WAITING_QUEUE_SIZE`、`EYAKV_MAX_WAITING_TIME`、`EYAKV_PASSWORD`、`EYAKV_WORKER_THREAD_COUNT`、`EYAKV_WORKER_QUEUE_SIZE`、`EYAKV_WORKER_WAIT_TIMEOUT`

Raft 选举配置：
- `EYAKV_RAFT_ELECTION_TIMEOUT_MIN_MS`、`EYAKV_RAFT_ELECTION_TIMEOUT_MAX_MS`、`EYAKV_RAFT_HEARTBEAT_INTERVAL_MS`、`EYAKV_RAFT_RPC_TIMEOUT_MS`、`EYAKV_RAFT_FOLLOWER_IDLE_WAIT_MS`、`EYAKV_RAFT_JOIN_MAX_RETRIES`

Raft 日志配置：
- `EYAKV_RAFT_REQUEST_VOTE_TIMEOUT_MS`、`EYAKV_RAFT_SUBMIT_TIMEOUT_MS`、`EYAKV_RAFT_APPEND_ENTRIES_MAX_BATCH`、`EYAKV_RAFT_LOG_SIZE_THRESHOLD`、`EYAKV_RAFT_LOG_TRUNCATE_RATIO`、`EYAKV_RAFT_WAL_FILENAME`、`EYAKV_RAFT_INDEX_FILENAME`

Raft 快照配置：
- `EYAKV_RAFT_SNAPSHOT_CHUNK_SIZE_BYTES`、`EYAKV_RAFT_RESULT_CACHE_CAPACITY`

Raft 线程池配置：
- `EYAKV_RAFT_THREADPOOL_WORKERS`、`EYAKV_RAFT_THREADPOOL_QUEUE_SIZE`、`EYAKV_RAFT_THREADPOOL_WAIT_TIMEOUT_MS`

#### 主要配置项说明

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `port` | 5210 | KV 服务监听端口 |
| `raft_port` | 5211 | Raft 服务监听端口 |
| `raft_trust_ip` | 127.0.0.1 | 首个节点的信任 IP（用于集群初始化） |
| `log_level` | 1 (INFO) | 日志级别：0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=FATAL |
| `memtable_size` | 1073741824 | MemTable 大小（1GB） |
| `wal_enable` | true | 是否启用 WAL（写前日志） |
| `raft_election_timeout_min_ms` | 150 | Raft 选举超时最小值（毫秒） |
| `raft_election_timeout_max_ms` | 300 | Raft 选举超时最大值（毫秒） |
| `raft_heartbeat_interval_ms` | 30 | Raft 心跳间隔（毫秒） |

### 启动方法

#### 单节点模式

```bash
# 启动服务器（默认配置）
cd build/bin
./eyakv_server
# 或 Windows: eyakv_server.exe
```

#### 集群模式（3 节点示例）

```bash
# 创建数据目录
mkdir -p data/node1 data/node2 data/node3

# 节点 1（初始节点）
cd build/bin
mkdir -p node1
./eyakv_server &
PID1=$!

# 节点 2（连接到节点 1）
cd build/bin
mkdir -p node2
export EYAKV_CONFIG_PATH=../conf/node2.conf
./eyakv_server &
PID2=$!

# 节点 3（连接到节点 1）
cd build/bin
mkdir -p node3
export EYAKV_CONFIG_PATH=../conf/node3.conf
./eyakv_server &
PID3=$!
```

#### 客户端连接

```bash
# 启动交互式客户端
cd build/bin
./eyakv

# 连接到指定服务器
./eyakv -h 127.0.0.1  -o 5210
```

#### 支持的命令

**基础 KV 操作：**
```
SET key value          # 设置键值对
GET key                # 获取值
DEL key1 key2 ...      # 删除多个键
EXISTS key             # 检查键是否存在
```

**复杂类型操作：**
```
# List（列表）
LPUSH key value1 value2 ...  # 从左侧推入
RPUSH key value1 value2 ...  # 从右侧推入
LPOP key [count]             # 从左侧弹出
RPOP key [count]             # 从右侧弹出
LRANGE key start stop        # 获取范围
LGET key index               # 获取指定索引的值
LLEN key                     # 获取列表长度

# Set（集合）
SADD key member1 member2 ... # 添加成员
SREM key member1 member2 ... # 移除成员
SMEMBERS key                 # 获取所有成员
SCARD key                    # 获取成员数量
SISMEMBER key member         # 检查成员是否存在

# Hash（哈希）
HSET key field value         # 设置字段
HGET key field               # 获取字段
HDEL key field1 field2 ...   # 删除字段
HKEYS key                    # 获取所有字段
HVALS key                    # 获取所有值
HGETALL key                  # 获取所有字段和值

# ZSet（有序集合）
ZADD key score1 member1 score2 member2 ...  # 添加成员
ZREM key member1 member2 ...                # 移除成员
ZRANGE key start stop                       # 获取范围（按分数排序）
ZRANGEBYSCORE key min max                   # 按分数范围获取
ZSCORE key member                           # 获取成员分数
ZCARD key                                    # 获取成员数量
```

**集群管理命令：**
```
SET_MASTER           # 设置当前节点为主节点（仅用于测试）
GET_LEADER           # 查询当前集群的 Leader
REMOVE_NODE			 # 移除节点
```

---

## 代码结构说明

### 目录结构

```
EyaKV/
├── apps/                    # 应用程序
│   ├── client/             # 客户端程序
│   │   ├── main.cpp        # 交互式客户端主程序
│   │   ├── test_client.cpp # 测试客户端
│   │   └── stress_test.cpp # 压力测试工具
│   └── server/             # 服务器程序
│       └── main.cpp        # 服务器入口
├── build/                  # 构建目录（自动生成）
│   ├── bin/               # 可执行文件
│   └── conf/              # 配置文件
├── include/               # 头文件
│   ├── common/            # 公共组件
│   │   ├── base/          # 基础宏和导出定义
│   │   ├── concurrency/   # 并发工具（ThreadPool）
│   │   ├── ds/            # 数据结构（SkipList）
│   │   ├── serialization/ # 序列化工具
│   │   ├── socket/        # Socket 封装（跨平台）
│   │   ├── types/         # 类型定义（EyaValue）
│   │   └── util/          # 工具函数
│   ├── config/            # 配置管理
│   │   └── config.h       # EyaKVConfig 配置类
│   ├── logger/            # 日志模块
│   │   └── logger.h       # 日志器封装（基于 glog）
│   ├── network/           # 网络模块
│   │   ├── protocol/      # 协议定义
│   │   │   └── protocol.h # Request/Response 协议
│   │   └── tcp_server.h   # TCP 服务器
│   ├── raft/              # Raft 共识模块
│   │   ├── protocol/      # Raft 协议定义
│   │   │   └── protocol.h # Raft 消息类型（RequestVote、AppendEntries）
│   │   └── raft.h         # RaftNode 类定义
│   ├── storage/           # 存储引擎
│   │   ├── processors/    # 数据类型处理器
│   │   ├── memtable.h     # MemTable（基于 SkipList）
│   │   ├── sstable.h      # SSTable 管理
│   │   ├── storage.h      # Storage 主类（统一入口）
│   │   └── wal.h          # WAL（写前日志）
│   └── starter/           # 系统启动
│       └── starter.h      # EyaKVStarter 启动器
├── src/                   # 源文件
│   ├── common/            # 公共组件实现
│   ├── logger/            # 日志实现
│   ├── network/           # 网络实现
│   │   └── tcp_server.cpp
│   ├── raft/              # Raft 实现
│   │   ├── raft.cpp       # RaftNode 核心逻辑（2600+ 行）
│   │   └── raft_log_array.cpp # Raft 日志数组
│   ├── storage/           # 存储实现
│   │   ├── memtable.cpp
│   │   ├── sstable.cpp
│   │   ├── storage.cpp
│   │   ├── wal.cpp
│   │   └── processors.cpp # 数据类型处理器实现
│   └── starter/           # 启动器实现
│       └── starter.cpp
├── tests/                 # 单元测试
│   └── CMakeLists.txt
├── CMakeLists.txt         # 根 CMake 配置
├── .gitignore             # Git 忽略文件
└── README.md              # 项目文档
```

### 核心模块解析

#### 1. 存储引擎模块（Storage Engine）

**LSM-Tree 架构设计：**

```
写入流程：
客户端请求 → WAL（持久化） → MemTable（内存索引） → Immutable MemTable → SSTable（磁盘）

读取流程：
MemTable → Immutable MemTables → SSTable（按时间倒序）
```

**核心组件：**

- **MemTable** (`memtable.h/cpp`)
  - 基于 **跳表（SkipList）** 实现，提供 O(log N) 的读写性能
  - 支持 TTL（生存时间）配置
  - 达到阈值后自动转换为 Immutable MemTable

- **WAL** (`wal.h/cpp`)
  - 写前日志，数据持久化保障
  - 三种刷新策略：后台线程定时、写入立即刷新、OS 缓冲
  - 支持崩溃恢复时重放日志

- **SSTable** (`sstable.h/cpp`)
  - 有序字符串表，磁盘持久化格式
  - 支持多种合并策略（大小分层、层级合并）
  - 自动后台 Compaction，减少空间放大

- **Storage** (`storage.h/cpp`)
  - 存储引擎统一入口（单例模式）
  - 管理所有 MemTable、WAL 和 SSTable
  - 提供 `GET`、`PUT`、`DELETE`、`EXPIRE` 等接口
  - 支持快照创建和恢复

#### 2. 网络模块（Network Module）

**Reactor 模式架构：**

```
主线程（Acceptor）
  ├─ 监听端口
  ├─ 接受新连接
  └─ 分发给 Worker 线程池

Worker 线程池
  ├─ 维护事件循环（epoll/kqueue/select）
  ├─ 读取数据、解析协议
  └─ 调用存储引擎执行逻辑
```

**核心组件：**

- **EyaServer** (`tcp_server.h/cpp`)
  - 跨平台 IO 复用（Linux epoll、macOS kqueue、Windows select）
  - 连接池管理（最大连接数、等待队列、超时控制）
  - 支持密码认证机制
  - 线程池处理客户端请求

- **ThreadPool** (`concurrency/threadpool.h`)
  - 生产者-消费者模式
  - 有界任务队列
  - 支持优雅关闭和强制关闭

- **Protocol** (`network/protocol/protocol.h`)
  - 自定义二进制协议
  - 支持多种返回类型（String、Vector、KV 对等）
  - 网络字节序转换

#### 3. Raft 共识模块（Raft Module）

**Raft 状态机：**

```
Follower（跟随者）
  ├─ 接收 AppendEntries（心跳/日志）
  ├─ 接收 RequestVote（投票）
  └─ 超时 → Candidate（候选人）

Candidate（候选人）
  ├─ 发起选举
  ├─ 请求投票
  ├─ 获得多数票 → Leader（领导者）
  └─ 发现更高 term → Follower

Leader（领导者）
  ├─ 处理客户端请求
  ├─ 复制日志到 Followers
  ├─ 周期性发送心跳
  └─ 提交已复制的日志
```

**核心组件：**

- **RaftNode** (`raft.h/cpp`，2600+ 行)
  - 完整的 Raft 协议实现
  - Leader 选举、日志复制、快照传输
  - 条件变量优化线程唤醒（避免频繁创建/销毁线程）
  - 指数退避重试机制（Follower 加入集群）

- **RaftLogArray** (`raft_log_array.cpp`)
  - Raft 日志的高效管理
  - 支持日志截断和压缩

- **Raft 协议** (`raft/protocol/protocol.h`)
  - 消息类型：RequestVote、AppendEntries、NewMaster、JoinCluster 等
  - LogEntry 序列化/反序列化

**关键优化：**
1. **条件变量优化**：选举线程和心跳线程使用条件变量等待，避免频繁创建/销毁线程
2. **任期检查**：广播新 Master 前验证 term，避免竞争条件
3. **指数退避重试**：Follower 加入集群时使用 2s、4s、6s 的超时重试策略

#### 4. 配置管理模块（Config Module）

**EyaKVConfig** (`config/config.h`)
- 单例模式设计
- 支持配置文件加载（键值对格式）
- 支持环境变量配置（`EYAKV_CONFIG_PATH`）
- 内置默认值和配置验证
- 涵盖日志、存储、网络、Raft 等所有配置项

#### 5. 启动模块（Starter Module）

**EyaKVStarter** (`starter/starter.h/cpp`)
- 系统启动入口
- 初始化顺序：
  1. 加载配置
  2. 初始化日志器
  3. 初始化存储引擎
  4. 初始化 Raft 节点
  5. 启动网络服务器
- 支持优雅关闭

#### 6. 公共组件（Common Module）

**核心组件：**

- **ThreadPool**：通用线程池实现
- **SkipList**：跳表数据结构（MemTable 基础）
- **Serializer**：序列化工具
- **TCPBase/TCPClient/TCPServer**：跨平台 Socket 封装
- **EyaValue**：值类型封装（支持 String、List、Set、Hash、ZSet）
