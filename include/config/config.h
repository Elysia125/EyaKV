/**
 * @file config.h
 * @brief EyaKV 全局配置管理模块
 * @details 负责从配置文件、环境变量及默认值中加载和管理系统运行所需的所有配置。
 *          配置加载优先级：环境变量 > 配置文件 > 默认配置。
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <optional>
#include <thread>
#include <filesystem>
#include <stdexcept>
#include "common/util/path_utils.h"
#include "common/util/string_utils.h"

// 避免与 Windows API 的 ERROR 宏冲突
#undef ERROR

/**
 * @brief 日志输出级别
 */
enum class LogLevel
{
    DEBUG = 0, ///< 调试信息，最详尽
    INFO,      ///< 普通信息，系统运行状态
    WARN,      ///< 警告，可能存在潜在问题
    ERROR,     ///< 错误，影响部分功能
    FATAL      ///< 致命错误，导致程序退出
};

/**
 * @brief WAL（预写日志）的刷新/刷盘策略
 */
enum class WALFlushStrategy
{
    BACKGROUND_THREAD = 0, ///< 后台线程定时异步刷新
    IMMEDIATE_ON_WRITE,    ///< 每次写入立刻同步刷盘（最安全，性能最低）
    OS_BUFFERED            ///< 写入系统内核缓冲区，依靠操作系统调度刷盘
};

/**
 * @brief SSTable 合并与压缩策略
 */
enum class SSTableMergeStrategy
{
    SIZE_TIERED_COMPACTION = 0, ///< 大小分层压缩 (读放大较高，写放大较低)
    LEVEL_COMPACTION,           ///< 层级合并 (读放大较低，写放大较高)
};

// ============================================================================
// 默认配置项常量 (使用 C++17 inline constexpr 替代不安全的 #define)
// ============================================================================
inline constexpr uint16_t DEFAULT_PORT = 5210;
inline constexpr const char *DEFAULT_IP = "0.0.0.0";
inline constexpr uint16_t DEFAULT_RAFT_PORT = 5211;
inline constexpr const char *DEFAULT_RAFT_TRUST_IP = "127.0.0.1";
inline constexpr bool DEFAULT_READ_ONLY = false;
inline constexpr LogLevel DEFAULT_LOG_LEVEL = LogLevel::INFO;
inline constexpr uint32_t DEFAULT_LOG_ROTATE_SIZE = 1024 * 1024; // KB
inline constexpr bool DEFAULT_LOG_CONSOLE_ENABLED = true;
inline constexpr int DEFAULT_SKIPLIST_MAX_LEVEL = 16;
inline constexpr double DEFAULT_SKIPLIST_PROBABILITY = 0.5;
inline constexpr uint32_t DEFAULT_SKIPLIST_MAX_NODE_COUNT = 10000000;
inline constexpr uint32_t DEFAULT_MEMTABLE_SIZE = 1024 * 1024; // KB
inline constexpr bool DEFAULT_WAL_ENABLE = true;
inline constexpr uint32_t DEFAULT_WAL_FILE_SIZE = 1024 * 1024 * 10; // KB
inline constexpr uint32_t DEFAULT_WAL_FILE_MAX_COUNT = 10;
inline constexpr uint32_t DEFAULT_WAL_FLUSH_INTERVAL = 1000; // ms
inline constexpr WALFlushStrategy DEFAULT_WAL_FLUSH_STRATEGY = WALFlushStrategy::BACKGROUND_THREAD;
inline constexpr SSTableMergeStrategy DEFAULT_SSTABLE_MERGE_STRATEGY = SSTableMergeStrategy::SIZE_TIERED_COMPACTION;
inline constexpr uint32_t DEFAULT_SSTABLE_MERGE_THRESHOLD = 5;
inline constexpr uint32_t DEFAULT_SSTABLE_ZERO_LEVEL_SIZE = 10; // MB
inline constexpr uint32_t DEFAULT_SSTABLE_LEVEL_SIZE_RATIO = 10;
inline constexpr uint32_t DEFAULT_MAX_CONNECTIONS = 10000;
inline constexpr uint32_t DEFAULT_MEMORY_POOL_SIZE = 1024 * 3;
inline constexpr uint32_t DEFAULT_WAITING_QUEUE_SIZE = 100;
inline constexpr uint32_t DEFAULT_MAX_WAITING_TIME = 30; // s
inline constexpr uint32_t DEFAULT_WORKER_QUEUE_SIZE = 1000;
inline constexpr uint32_t DEFAULT_WORKER_WAIT_TIMEOUT = 30;
// thread count 需要运行时获取，故仅使用 inline const
inline const unsigned int DEFAULT_WORKER_THREAD_COUNT = std::thread::hardware_concurrency() + 1;

// Raft 相关默认配置
inline constexpr uint32_t DEFAULT_RAFT_ELECTION_TIMEOUT_MIN = 150; // ms
inline constexpr uint32_t DEFAULT_RAFT_ELECTION_TIMEOUT_MAX = 300; // ms
inline constexpr uint32_t DEFAULT_RAFT_HEARTBEAT_INTERVAL = 30;    // ms
inline constexpr uint32_t DEFAULT_RAFT_RPC_TIMEOUT = 2000;         // ms
inline constexpr uint32_t DEFAULT_RAFT_FOLLOWER_IDLE_WAIT = 1000;  // ms
inline constexpr uint32_t DEFAULT_RAFT_JOIN_MAX_RETRIES = 3;
inline constexpr uint32_t DEFAULT_RAFT_REQUEST_VOTE_TIMEOUT = 200; // ms
inline constexpr uint32_t DEFAULT_RAFT_SUBMIT_TIMEOUT = 500;       // ms
inline constexpr uint32_t DEFAULT_RAFT_APPEND_BATCH = 1000;
inline constexpr uint32_t DEFAULT_RAFT_SNAPSHOT_CHUNK = 64 * 1024; // Bytes
inline constexpr uint32_t DEFAULT_RAFT_RESULT_CACHE_CAPACITY = 10000;
inline constexpr uint32_t DEFAULT_RAFT_THREADPOOL_WORKERS = 4;
inline constexpr uint32_t DEFAULT_RAFT_THREADPOOL_QUEUE = 10000;
inline constexpr uint32_t DEFAULT_RAFT_THREADPOOL_WAIT = 1000; // ms
inline constexpr uint32_t DEFAULT_RAFT_LOG_THRESHOLD = 1000000;
inline constexpr double DEFAULT_RAFT_LOG_TRUNCATE_RATIO = 0.25;
inline constexpr const char *DEFAULT_RAFT_WAL_FILENAME = "raft_wal.log";
inline constexpr const char *DEFAULT_RAFT_INDEX_FILENAME = "raft_index.idx";
inline constexpr bool DEFAULT_RAFT_NEED_MAJORITY_CONFIRM = false;
inline constexpr uint32_t DEFAULT_BATCH_TIMEOUT_MS = 500;

// ============================================================================
// 配置字典键名常量 (Keys)
// ============================================================================
inline constexpr const char *PORT_KEY = "port";
inline constexpr const char *IP_KEY = "ip";
inline constexpr const char *RAFT_PORT_KEY = "raft_port";
inline constexpr const char *RAFT_TRUST_IP_KEY = "raft_trust_ip";
inline constexpr const char *READ_ONLY_KEY = "read_only";
inline constexpr const char *LOG_LEVEL_KEY = "log_level";
inline constexpr const char *LOG_ROTATE_SIZE_KEY = "log_rotate_size";
inline constexpr const char *LOG_CONSOLE_ENABLED_KEY = "log_console_enabled";
inline constexpr const char *SKIPLIST_MAX_LEVEL_KEY = "skiplist_max_level";
inline constexpr const char *SKIPLIST_PROBABILITY_KEY = "skiplist_probability";
inline constexpr const char *SKIPLIST_MAX_NODE_COUNT_KEY = "skiplist_max_node_count";
inline constexpr const char *MEMTABLE_SIZE_KEY = "memtable_size";
inline constexpr const char *WAL_ENABLE_KEY = "wal_enable";
inline constexpr const char *WAL_DIR_KEY = "wal_dir";
inline constexpr const char *WAL_FILE_SIZE_KEY = "wal_file_size";
inline constexpr const char *WAL_FILE_MAX_COUNT_KEY = "wal_file_max_count";
inline constexpr const char *WAL_FLUSH_INTERVAL_KEY = "wal_flush_interval";
inline constexpr const char *WAL_FLUSH_STRATEGY_KEY = "wal_flush_strategy";
inline constexpr const char *SSTABLE_MERGE_STRATEGY_KEY = "sstable_merge_strategy";
inline constexpr const char *SSTABLE_ZERO_LEVEL_SIZE_KEY = "sstable_zero_level_size";
inline constexpr const char *SSTABLE_LEVEL_SIZE_RATIO_KEY = "sstable_level_size_ratio";
inline constexpr const char *SSTABLE_MERGE_THRESHOLD_KEY = "sstable_merge_threshold";
inline constexpr const char *MAX_CONNECTIONS_KEY = "max_connections";
inline constexpr const char *MEMORY_POOL_SIZE_KEY = "memory_pool_size";
inline constexpr const char *WAITING_QUEUE_SIZE_KEY = "waiting_queue_size";
inline constexpr const char *MAX_WAITING_TIME_KEY = "max_waiting_time";
inline constexpr const char *LOG_DIR_KEY = "log_dir";
inline constexpr const char *DATA_DIR_KEY = "data_dir";
inline constexpr const char *PASSWORD_KEY = "password";
inline constexpr const char *WORKER_THREAD_COUNT_KEY = "worker_thread_count";
inline constexpr const char *WORKER_QUEUE_SIZE_KEY = "worker_queue_size";
inline constexpr const char *WORKER_WAIT_TIMEOUT_KEY = "worker_wait_timeout";

inline constexpr const char *RAFT_ELECTION_TIMEOUT_MIN_KEY = "raft_election_timeout_min_ms";
inline constexpr const char *RAFT_ELECTION_TIMEOUT_MAX_KEY = "raft_election_timeout_max_ms";
inline constexpr const char *RAFT_HEARTBEAT_INTERVAL_KEY = "raft_heartbeat_interval_ms";
inline constexpr const char *RAFT_RPC_TIMEOUT_KEY = "raft_rpc_timeout_ms";
inline constexpr const char *RAFT_FOLLOWER_IDLE_WAIT_KEY = "raft_follower_idle_wait_ms";
inline constexpr const char *RAFT_JOIN_MAX_RETRIES_KEY = "raft_join_max_retries";
inline constexpr const char *RAFT_REQUEST_VOTE_TIMEOUT_KEY = "raft_request_vote_timeout_ms";
inline constexpr const char *RAFT_SUBMIT_TIMEOUT_KEY = "raft_submit_timeout_ms";
inline constexpr const char *RAFT_APPEND_BATCH_KEY = "raft_append_entries_max_batch";
inline constexpr const char *RAFT_SNAPSHOT_CHUNK_KEY = "raft_snapshot_chunk_size_bytes";
inline constexpr const char *RAFT_RESULT_CACHE_CAPACITY_KEY = "raft_result_cache_capacity";
inline constexpr const char *RAFT_THREADPOOL_WORKERS_KEY = "raft_threadpool_workers";
inline constexpr const char *RAFT_THREADPOOL_QUEUE_KEY = "raft_threadpool_queue_size";
inline constexpr const char *RAFT_THREADPOOL_WAIT_KEY = "raft_threadpool_wait_timeout_ms";
inline constexpr const char *RAFT_LOG_THRESHOLD_KEY = "raft_log_size_threshold";
inline constexpr const char *RAFT_LOG_TRUNCATE_RATIO_KEY = "raft_log_truncate_ratio";
inline constexpr const char *RAFT_WAL_FILENAME_KEY = "raft_wal_filename";
inline constexpr const char *RAFT_INDEX_FILENAME_KEY = "raft_index_filename";
inline constexpr const char *RAFT_NEED_MAJORITY_CONFIRM_KEY = "raft_need_majority_confirm";
inline constexpr const char *BATCH_TIMEOUT_KEY = "batch_timeout";
/**
 * @class EyaKVConfig
 * @brief 系统全局配置管理器 (单例)
 */
class EyaKVConfig
{
public:
    // 禁用拷贝和赋值，保障单例安全性
    EyaKVConfig(const EyaKVConfig &) = delete;
    void operator=(const EyaKVConfig &) = delete;

    /**
     * @brief 获取全局配置单例实例
     * @return EyaKVConfig& 实例引用
     */
    static EyaKVConfig &get_instance()
    {
        static EyaKVConfig instance;
        return instance;
    }

    /**
     * @brief 根据键名获取配置项
     * @param key 配置键名
     * @return std::optional<std::string> 配置项的字符串值，如果未找到则返回 nullopt
     */
    std::optional<std::string> get_config(const std::string &key) const
    {
        // C++17 的初始化 if 语法，减少了一次哈希查找，极大提高性能
        if (auto it = config_map_.find(key); it != config_map_.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

private:
    std::string config_file_;
    std::unordered_map<std::string, std::string> config_map_;

    /**
     * @brief 配置 Key 映射到 环境变量 Key 的静态常量表
     * @details 使用 C++17 inline static 特性直接类内初始化，摒弃外部函数
     */
    inline static const std::unordered_map<std::string, std::string> ENV_KEY_MAP = {
        {PORT_KEY, "EYAKV_PORT"},
        {IP_KEY, "EYAKV_IP"},
        {RAFT_PORT_KEY, "EYAKV_RAFT_PORT"},
        {RAFT_TRUST_IP_KEY, "EYAKV_RAFT_TRUST_IP"},
        {LOG_LEVEL_KEY, "EYAKV_LOG_LEVEL"},
        {LOG_ROTATE_SIZE_KEY, "EYAKV_LOG_ROTATE_SIZE"},
        {LOG_DIR_KEY, "EYAKV_LOG_DIR"},
        {LOG_CONSOLE_ENABLED_KEY, "EYAKV_LOG_CONSOLE_ENABLED"},
        {READ_ONLY_KEY, "EYAKV_READ_ONLY"},
        {MEMTABLE_SIZE_KEY, "EYAKV_MEMTABLE_SIZE"},
        {DATA_DIR_KEY, "EYAKV_DATA_DIR"},
        {SKIPLIST_MAX_LEVEL_KEY, "EYAKV_SKIPLIST_MAX_LEVEL"},
        {SKIPLIST_PROBABILITY_KEY, "EYAKV_SKIPLIST_PROBABILITY"},
        {SKIPLIST_MAX_NODE_COUNT_KEY, "EYAKV_SKIPLIST_MAX_NODE_COUNT"},
        {WAL_ENABLE_KEY, "EYAKV_WAL_ENABLE"},
        {WAL_DIR_KEY, "EYAKV_WAL_DIR"},
        {WAL_FILE_SIZE_KEY, "EYAKV_WAL_FILE_SIZE"},
        {WAL_FILE_MAX_COUNT_KEY, "EYAKV_WAL_FILE_MAX_COUNT"},
        {WAL_FLUSH_INTERVAL_KEY, "EYAKV_WAL_FLUSH_INTERVAL"},
        {WAL_FLUSH_STRATEGY_KEY, "EYAKV_WAL_FLUSH_STRATEGY"},
        {SSTABLE_MERGE_STRATEGY_KEY, "EYAKV_SSTABLE_MERGE_STRATEGY"},
        {SSTABLE_ZERO_LEVEL_SIZE_KEY, "EYAKV_SSTABLE_ZERO_LEVEL_SIZE"},
        {SSTABLE_LEVEL_SIZE_RATIO_KEY, "EYAKV_SSTABLE_LEVEL_SIZE_RATIO"},
        {SSTABLE_MERGE_THRESHOLD_KEY, "EYAKV_SSTABLE_MERGE_THRESHOLD"},
        {MAX_CONNECTIONS_KEY, "EYAKV_MAX_CONNECTIONS"},
        {MEMORY_POOL_SIZE_KEY, "EYAKV_MEMORY_POOL_SIZE"},
        {WAITING_QUEUE_SIZE_KEY, "EYAKV_WAITING_QUEUE_SIZE"},
        {MAX_WAITING_TIME_KEY, "EYAKV_MAX_WAITING_TIME"},
        {PASSWORD_KEY, "EYAKV_PASSWORD"},
        {WORKER_THREAD_COUNT_KEY, "EYAKV_WORKER_THREAD_COUNT"},
        {WORKER_QUEUE_SIZE_KEY, "EYAKV_WORKER_QUEUE_SIZE"},
        {WORKER_WAIT_TIMEOUT_KEY, "EYAKV_WORKER_WAIT_TIMEOUT"},
        {RAFT_ELECTION_TIMEOUT_MIN_KEY, "EYAKV_RAFT_ELECTION_TIMEOUT_MIN_MS"},
        {RAFT_ELECTION_TIMEOUT_MAX_KEY, "EYAKV_RAFT_ELECTION_TIMEOUT_MAX_MS"},
        {RAFT_HEARTBEAT_INTERVAL_KEY, "EYAKV_RAFT_HEARTBEAT_INTERVAL_MS"},
        {RAFT_RPC_TIMEOUT_KEY, "EYAKV_RAFT_RPC_TIMEOUT_MS"},
        {RAFT_FOLLOWER_IDLE_WAIT_KEY, "EYAKV_RAFT_FOLLOWER_IDLE_WAIT_MS"},
        {RAFT_JOIN_MAX_RETRIES_KEY, "EYAKV_RAFT_JOIN_MAX_RETRIES"},
        {RAFT_REQUEST_VOTE_TIMEOUT_KEY, "EYAKV_RAFT_REQUEST_VOTE_TIMEOUT_MS"},
        {RAFT_SUBMIT_TIMEOUT_KEY, "EYAKV_RAFT_SUBMIT_TIMEOUT_MS"},
        {RAFT_APPEND_BATCH_KEY, "EYAKV_RAFT_APPEND_ENTRIES_MAX_BATCH"},
        {RAFT_LOG_THRESHOLD_KEY, "EYAKV_RAFT_LOG_SIZE_THRESHOLD"},
        {RAFT_LOG_TRUNCATE_RATIO_KEY, "EYAKV_RAFT_LOG_TRUNCATE_RATIO"},
        {RAFT_WAL_FILENAME_KEY, "EYAKV_RAFT_WAL_FILENAME"},
        {RAFT_INDEX_FILENAME_KEY, "EYAKV_RAFT_INDEX_FILENAME"},
        {RAFT_NEED_MAJORITY_CONFIRM_KEY, "EYAKV_RAFT_NEED_MAJORITY_CONFIRM"},
        {RAFT_SNAPSHOT_CHUNK_KEY, "EYAKV_RAFT_SNAPSHOT_CHUNK_SIZE_BYTES"},
        {RAFT_RESULT_CACHE_CAPACITY_KEY, "EYAKV_RAFT_RESULT_CACHE_CAPACITY"},
        {RAFT_THREADPOOL_WORKERS_KEY, "EYAKV_RAFT_THREADPOOL_WORKERS"},
        {RAFT_THREADPOOL_QUEUE_KEY, "EYAKV_RAFT_THREADPOOL_QUEUE_SIZE"},
        {RAFT_THREADPOOL_WAIT_KEY, "EYAKV_RAFT_THREADPOOL_WAIT_TIMEOUT_MS"},
        {BATCH_TIMEOUT_KEY, "EYAKV_BATCH_TIMEOUT"}};

    /**
     * @brief 构造函数：按照 默认 -> 配置文件 -> 环境变量 顺序加载并覆盖
     */
    EyaKVConfig()
    {
        if (const char *env_config = std::getenv("EYAKV_CONFIG_PATH"))
        {
            config_file_ = env_config;
        }
        else
        {
            config_file_ = PathUtils::combine_path(PathUtils::get_target_file_path("conf"), "eyakv.conf");
        }

        load_default_config();
        load_config();
        load_config_from_env();
        check_config();

        // 配置文件不存在时生成默认配置文件
        if (!std::filesystem::exists(config_file_))
        {
            generate_default_config_file();
        }
    }

    /**
     * @brief 生成带注释的默认配置文件
     * @details 自动创建配置文件所在目录，并写入所有默认配置（带中文注释说明）
     * @throw std::runtime_error 目录创建失败/文件写入失败时抛出
     */
    void generate_default_config_file()
    {
        // 1. 创建配置文件所在目录（如 conf/）
        std::filesystem::path config_dir = std::filesystem::path(config_file_).parent_path();
        if (!std::filesystem::exists(config_dir))
        {
            if (!std::filesystem::create_directories(config_dir))
            {
                throw std::runtime_error("Failed to create config directory: " + config_dir.string());
            }
        }

        // 2. 打开配置文件并写入默认配置（带注释）
        std::ofstream config_file(config_file_, std::ios::out | std::ios::trunc);
        if (!config_file.is_open())
        {
            throw std::runtime_error("Failed to create default config file: " + config_file_);
        }

        // 写入配置文件头部说明
        config_file << "# ===================== EyaKV 配置文件 =====================\n";
        config_file << "# 配置加载优先级：环境变量 > 配置文件 > 默认配置\n";
        config_file << "# 环境变量命名规则：配置项KEY大写 + EYAKV_前缀（如 port -> EYAKV_PORT）\n";
        config_file << "# 所有数值单位未特殊说明时：大小为KB，时间为ms\n\n";

        // 基础网络配置
        config_file << "# -------------------- 基础网络配置 --------------------\n";
        config_file << IP_KEY << "=" << DEFAULT_IP << "  # 服务监听IP\n";
        config_file << PORT_KEY << "=" << DEFAULT_PORT << "  # 服务监听端口\n";
        config_file << RAFT_PORT_KEY << "=" << DEFAULT_RAFT_PORT << "  # Raft通信端口\n";
        config_file << RAFT_TRUST_IP_KEY << "=" << DEFAULT_RAFT_TRUST_IP << "  # Raft信任IP\n";
        config_file << MAX_CONNECTIONS_KEY << "=" << DEFAULT_MAX_CONNECTIONS << "  # 最大并发连接数\n";
        config_file << READ_ONLY_KEY << "=" << (DEFAULT_READ_ONLY ? "true" : "false") << "  # 是否只读模式\n\n";

        // 日志配置
        config_file << "# -------------------- 日志配置 --------------------\n";
        config_file << LOG_LEVEL_KEY << "=" << static_cast<int>(DEFAULT_LOG_LEVEL) << "  # 日志级别(0=DEBUG,1=INFO,2=WARN,3=ERROR,4=FATAL)\n";
        config_file << LOG_ROTATE_SIZE_KEY << "=" << DEFAULT_LOG_ROTATE_SIZE << "  # 日志文件轮转大小(KB)\n";
        config_file << LOG_CONSOLE_ENABLED_KEY << "=" << (DEFAULT_LOG_CONSOLE_ENABLED ? "true" : "false") << "  # 是否开启控制台日志\n";
        config_file << LOG_DIR_KEY << "=" << PathUtils::get_target_file_path("logs") << "  # 日志文件存储目录\n\n";

        // 跳表配置
        config_file << "# -------------------- 跳表配置 --------------------\n";
        config_file << SKIPLIST_MAX_LEVEL_KEY << "=" << DEFAULT_SKIPLIST_MAX_LEVEL << "  # 跳表最大层级\n";
        config_file << SKIPLIST_PROBABILITY_KEY << "=" << DEFAULT_SKIPLIST_PROBABILITY << "  # 跳表层级晋升概率(0-1)\n";
        config_file << SKIPLIST_MAX_NODE_COUNT_KEY << "=" << DEFAULT_SKIPLIST_MAX_NODE_COUNT << "  # 跳表最大节点数\n\n";

        // 内存表配置
        config_file << "# -------------------- 内存表配置 --------------------\n";
        config_file << MEMTABLE_SIZE_KEY << "=" << DEFAULT_MEMTABLE_SIZE << "  # 内存表最大大小(KB)\n\n";

        // WAL 配置
        config_file << "# -------------------- WAL 预写日志配置 --------------------\n";
        config_file << WAL_ENABLE_KEY << "=" << (DEFAULT_WAL_ENABLE ? "true" : "false") << "  # 是否开启WAL\n";
        config_file << WAL_DIR_KEY << "=" << PathUtils::combine_path(PathUtils::get_target_file_path("data"), "wal") << "  # WAL文件存储目录\n";
        config_file << WAL_FILE_SIZE_KEY << "=" << DEFAULT_WAL_FILE_SIZE << "  # 单个WAL文件大小(KB)\n";
        config_file << WAL_FILE_MAX_COUNT_KEY << "=" << DEFAULT_WAL_FILE_MAX_COUNT << "  # WAL文件最大保留数量\n";
        config_file << WAL_FLUSH_INTERVAL_KEY << "=" << DEFAULT_WAL_FLUSH_INTERVAL << "  # WAL后台刷新间隔(ms)\n";
        config_file << WAL_FLUSH_STRATEGY_KEY << "=" << static_cast<int>(DEFAULT_WAL_FLUSH_STRATEGY) << "  # WAL刷盘策略(0=后台线程,1=立即刷盘,2=系统缓冲)\n\n";

        // SSTable 配置
        config_file << "# -------------------- SSTable 配置 --------------------\n";
        config_file << SSTABLE_MERGE_STRATEGY_KEY << "=" << static_cast<int>(DEFAULT_SSTABLE_MERGE_STRATEGY) << "  # 合并策略(0=大小分层,1=层级合并)\n";
        config_file << SSTABLE_MERGE_THRESHOLD_KEY << "=" << DEFAULT_SSTABLE_MERGE_THRESHOLD << "  # SSTable合并阈值\n";
        config_file << SSTABLE_ZERO_LEVEL_SIZE_KEY << "=" << DEFAULT_SSTABLE_ZERO_LEVEL_SIZE << "  # 0级SSTable总大小(MB)\n";
        config_file << SSTABLE_LEVEL_SIZE_RATIO_KEY << "=" << DEFAULT_SSTABLE_LEVEL_SIZE_RATIO << "  # 各级SSTable大小比例\n\n";

        // 线程池与并发配置
        config_file << "# -------------------- 线程池与并发配置 --------------------\n";
        config_file << WORKER_THREAD_COUNT_KEY << "=" << DEFAULT_WORKER_THREAD_COUNT << "  # 工作线程数(默认=CPU核心数+1)\n";
        config_file << WORKER_QUEUE_SIZE_KEY << "=" << DEFAULT_WORKER_QUEUE_SIZE << "  # 工作队列大小\n";
        config_file << WORKER_WAIT_TIMEOUT_KEY << "=" << DEFAULT_WORKER_WAIT_TIMEOUT << "  # 工作线程等待超时(s)\n";
        config_file << MEMORY_POOL_SIZE_KEY << "=" << DEFAULT_MEMORY_POOL_SIZE << "  # 内存池大小(KB)\n";
        config_file << WAITING_QUEUE_SIZE_KEY << "=" << DEFAULT_WAITING_QUEUE_SIZE << "  # 请求等待队列大小\n";
        config_file << MAX_WAITING_TIME_KEY << "=" << DEFAULT_MAX_WAITING_TIME << "  # 请求最大等待时间(s)\n\n";

        // Raft 配置
        config_file << "# -------------------- Raft 共识协议配置 --------------------\n";
        config_file << RAFT_ELECTION_TIMEOUT_MIN_KEY << "=" << DEFAULT_RAFT_ELECTION_TIMEOUT_MIN << "  # 选举超时最小值(ms)\n";
        config_file << RAFT_ELECTION_TIMEOUT_MAX_KEY << "=" << DEFAULT_RAFT_ELECTION_TIMEOUT_MAX << "  # 选举超时最大值(ms)\n";
        config_file << RAFT_HEARTBEAT_INTERVAL_KEY << "=" << DEFAULT_RAFT_HEARTBEAT_INTERVAL << "  # 心跳间隔(ms)\n";
        config_file << RAFT_RPC_TIMEOUT_KEY << "=" << DEFAULT_RAFT_RPC_TIMEOUT << "  # Raft RPC超时(ms)\n";
        config_file << RAFT_FOLLOWER_IDLE_WAIT_KEY << "=" << DEFAULT_RAFT_FOLLOWER_IDLE_WAIT << "  # Follower空闲等待时间(ms)\n";
        config_file << RAFT_JOIN_MAX_RETRIES_KEY << "=" << DEFAULT_RAFT_JOIN_MAX_RETRIES << "  # 加入集群最大重试次数\n";
        config_file << RAFT_REQUEST_VOTE_TIMEOUT_KEY << "=" << DEFAULT_RAFT_REQUEST_VOTE_TIMEOUT << "  # 请求投票超时(ms)\n";
        config_file << RAFT_SUBMIT_TIMEOUT_KEY << "=" << DEFAULT_RAFT_SUBMIT_TIMEOUT << "  # 日志提交超时(ms)\n";
        config_file << RAFT_APPEND_BATCH_KEY << "=" << DEFAULT_RAFT_APPEND_BATCH << "  # 追加日志批量大小\n";
        config_file << RAFT_SNAPSHOT_CHUNK_KEY << "=" << DEFAULT_RAFT_SNAPSHOT_CHUNK << "  # 快照分块大小(Bytes)\n";
        config_file << RAFT_RESULT_CACHE_CAPACITY_KEY << "=" << DEFAULT_RAFT_RESULT_CACHE_CAPACITY << "  # 结果缓存容量\n";
        config_file << RAFT_THREADPOOL_WORKERS_KEY << "=" << DEFAULT_RAFT_THREADPOOL_WORKERS << "  # Raft线程池工作线程数\n";
        config_file << RAFT_THREADPOOL_QUEUE_KEY << "=" << DEFAULT_RAFT_THREADPOOL_QUEUE << "  # Raft线程池队列大小\n";
        config_file << RAFT_THREADPOOL_WAIT_KEY << "=" << DEFAULT_RAFT_THREADPOOL_WAIT << "  # Raft线程池等待超时(ms)\n";
        config_file << RAFT_LOG_THRESHOLD_KEY << "=" << DEFAULT_RAFT_LOG_THRESHOLD << "  # Raft日志截断阈值\n";
        config_file << RAFT_LOG_TRUNCATE_RATIO_KEY << "=" << DEFAULT_RAFT_LOG_TRUNCATE_RATIO << "  # Raft日志截断比例\n";
        config_file << RAFT_WAL_FILENAME_KEY << "=" << DEFAULT_RAFT_WAL_FILENAME << "  # Raft WAL文件名\n";
        config_file << RAFT_INDEX_FILENAME_KEY << "=" << DEFAULT_RAFT_INDEX_FILENAME << "  # Raft索引文件名\n";
        config_file << RAFT_NEED_MAJORITY_CONFIRM_KEY << "=" << (DEFAULT_RAFT_NEED_MAJORITY_CONFIRM ? "true" : "false") << "  # 是否需要多数节点确认\n\n";

        // 批处理配置
        config_file << "# -------------------- 批处理配置 --------------------\n";
        config_file << BATCH_TIMEOUT_KEY << "=" << DEFAULT_BATCH_TIMEOUT_MS << "  # 批处理超时时间(ms)\n\n";

        // 安全配置
        config_file << "# -------------------- 安全配置 --------------------\n";
        config_file << "#" << PASSWORD_KEY << "=" << "" << "  # 服务访问密码(注释则无密码)\n";

        // 刷新并关闭文件
        config_file.flush();
        config_file.close();

        if (config_file.fail())
        {
            throw std::runtime_error("Failed to write default config file: " + config_file_);
        }
    }
    ~EyaKVConfig() = default;

    /**
     * @brief 从指定文件解析加载配置
     */
    void load_config()
    {
        if (!std::filesystem::exists(config_file_))
            return;

        std::ifstream config_file(config_file_);
        if (!config_file.is_open())
        {
            throw std::runtime_error("Failed to open config file: " + config_file_);
        }

        std::string line;
        while (std::getline(config_file, line))
        {
            size_t comment_idx = line.find('#');
            if (comment_idx != std::string::npos)
            {
                line.erase(comment_idx); // 避免生成新的字符串副本
            }
            if (line.empty())
                continue;

            size_t eq_idx = line.find('=');
            if (eq_idx == std::string::npos)
            {
                throw std::runtime_error("Invalid config line (missing '='): " + line);
            }

            std::string key = trim(line.substr(0, eq_idx));
            std::string value = trim(line.substr(eq_idx + 1));
            if (key.empty() || value.empty())
            {
                throw std::runtime_error("Invalid config line (empty key or value): " + line);
            }
            config_map_[key] = std::move(value);
        }
    }

    /**
     * @brief 从系统环境变量中加载配置项
     */
    void load_config_from_env()
    {
        for (const auto &[config_key, env_key] : ENV_KEY_MAP)
        {
            if (const char *env_value = std::getenv(env_key.c_str()); env_value && env_value[0] != '\0')
            {
                config_map_[config_key] = env_value;
            }
        }
    }

    /**
     * @brief 初始化默认字典
     */
    void load_default_config()
    {
        config_map_[LOG_DIR_KEY] = PathUtils::get_target_file_path("logs");
        config_map_[DATA_DIR_KEY] = PathUtils::get_target_file_path("data");
        config_map_[WAL_DIR_KEY] = PathUtils::combine_path(PathUtils::get_target_file_path("data"), "wal");
        config_map_[LOG_CONSOLE_ENABLED_KEY] = DEFAULT_LOG_CONSOLE_ENABLED ? "true" : "false";
        config_map_[LOG_LEVEL_KEY] = std::to_string(static_cast<int>(DEFAULT_LOG_LEVEL));
        config_map_[LOG_ROTATE_SIZE_KEY] = std::to_string(DEFAULT_LOG_ROTATE_SIZE);
        config_map_[IP_KEY] = DEFAULT_IP;
        config_map_[PORT_KEY] = std::to_string(DEFAULT_PORT);
        config_map_[READ_ONLY_KEY] = DEFAULT_READ_ONLY ? "true" : "false";
        config_map_[PASSWORD_KEY] = "";

        config_map_[SKIPLIST_MAX_LEVEL_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_LEVEL);
        config_map_[SKIPLIST_PROBABILITY_KEY] = std::to_string(DEFAULT_SKIPLIST_PROBABILITY);
        config_map_[SKIPLIST_MAX_NODE_COUNT_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_NODE_COUNT);

        config_map_[MEMTABLE_SIZE_KEY] = std::to_string(DEFAULT_MEMTABLE_SIZE);
        config_map_[WAL_ENABLE_KEY] = DEFAULT_WAL_ENABLE ? "true" : "false";
        config_map_[WAL_FILE_SIZE_KEY] = std::to_string(DEFAULT_WAL_FILE_SIZE);
        config_map_[WAL_FILE_MAX_COUNT_KEY] = std::to_string(DEFAULT_WAL_FILE_MAX_COUNT);
        config_map_[WAL_FLUSH_STRATEGY_KEY] = std::to_string(static_cast<int>(DEFAULT_WAL_FLUSH_STRATEGY));
        config_map_[WAL_FLUSH_INTERVAL_KEY] = std::to_string(DEFAULT_WAL_FLUSH_INTERVAL);

        config_map_[SSTABLE_MERGE_STRATEGY_KEY] = std::to_string(static_cast<int>(DEFAULT_SSTABLE_MERGE_STRATEGY));
        config_map_[SSTABLE_MERGE_THRESHOLD_KEY] = std::to_string(DEFAULT_SSTABLE_MERGE_THRESHOLD);
        config_map_[SSTABLE_ZERO_LEVEL_SIZE_KEY] = std::to_string(DEFAULT_SSTABLE_ZERO_LEVEL_SIZE);
        config_map_[SSTABLE_LEVEL_SIZE_RATIO_KEY] = std::to_string(DEFAULT_SSTABLE_LEVEL_SIZE_RATIO);

        config_map_[MAX_CONNECTIONS_KEY] = std::to_string(DEFAULT_MAX_CONNECTIONS);
        config_map_[MEMORY_POOL_SIZE_KEY] = std::to_string(DEFAULT_MEMORY_POOL_SIZE);
        config_map_[WAITING_QUEUE_SIZE_KEY] = std::to_string(DEFAULT_WAITING_QUEUE_SIZE);
        config_map_[MAX_WAITING_TIME_KEY] = std::to_string(DEFAULT_MAX_WAITING_TIME);

        config_map_[WORKER_THREAD_COUNT_KEY] = std::to_string(DEFAULT_WORKER_THREAD_COUNT);
        config_map_[WORKER_QUEUE_SIZE_KEY] = std::to_string(DEFAULT_WORKER_QUEUE_SIZE);
        config_map_[WORKER_WAIT_TIMEOUT_KEY] = std::to_string(DEFAULT_WORKER_WAIT_TIMEOUT);
        config_map_[BATCH_TIMEOUT_KEY] = std::to_string(DEFAULT_BATCH_TIMEOUT_MS);

        // Raft 默认参数配置
        config_map_[RAFT_PORT_KEY] = std::to_string(DEFAULT_RAFT_PORT);
        config_map_[RAFT_TRUST_IP_KEY] = DEFAULT_RAFT_TRUST_IP;
        config_map_[RAFT_ELECTION_TIMEOUT_MIN_KEY] = std::to_string(DEFAULT_RAFT_ELECTION_TIMEOUT_MIN);
        config_map_[RAFT_ELECTION_TIMEOUT_MAX_KEY] = std::to_string(DEFAULT_RAFT_ELECTION_TIMEOUT_MAX);
        config_map_[RAFT_HEARTBEAT_INTERVAL_KEY] = std::to_string(DEFAULT_RAFT_HEARTBEAT_INTERVAL);
        config_map_[RAFT_RPC_TIMEOUT_KEY] = std::to_string(DEFAULT_RAFT_RPC_TIMEOUT);
        config_map_[RAFT_FOLLOWER_IDLE_WAIT_KEY] = std::to_string(DEFAULT_RAFT_FOLLOWER_IDLE_WAIT);
        config_map_[RAFT_JOIN_MAX_RETRIES_KEY] = std::to_string(DEFAULT_RAFT_JOIN_MAX_RETRIES);
        config_map_[RAFT_REQUEST_VOTE_TIMEOUT_KEY] = std::to_string(DEFAULT_RAFT_REQUEST_VOTE_TIMEOUT);
        config_map_[RAFT_SUBMIT_TIMEOUT_KEY] = std::to_string(DEFAULT_RAFT_SUBMIT_TIMEOUT);
        config_map_[RAFT_APPEND_BATCH_KEY] = std::to_string(DEFAULT_RAFT_APPEND_BATCH);
        config_map_[RAFT_SNAPSHOT_CHUNK_KEY] = std::to_string(DEFAULT_RAFT_SNAPSHOT_CHUNK);
        config_map_[RAFT_RESULT_CACHE_CAPACITY_KEY] = std::to_string(DEFAULT_RAFT_RESULT_CACHE_CAPACITY);
        config_map_[RAFT_THREADPOOL_WORKERS_KEY] = std::to_string(DEFAULT_RAFT_THREADPOOL_WORKERS);
        config_map_[RAFT_THREADPOOL_QUEUE_KEY] = std::to_string(DEFAULT_RAFT_THREADPOOL_QUEUE);
        config_map_[RAFT_THREADPOOL_WAIT_KEY] = std::to_string(DEFAULT_RAFT_THREADPOOL_WAIT);
        config_map_[RAFT_LOG_THRESHOLD_KEY] = std::to_string(DEFAULT_RAFT_LOG_THRESHOLD);
        config_map_[RAFT_LOG_TRUNCATE_RATIO_KEY] = std::to_string(DEFAULT_RAFT_LOG_TRUNCATE_RATIO);
        config_map_[RAFT_WAL_FILENAME_KEY] = DEFAULT_RAFT_WAL_FILENAME;
        config_map_[RAFT_INDEX_FILENAME_KEY] = DEFAULT_RAFT_INDEX_FILENAME;
        config_map_[RAFT_NEED_MAJORITY_CONFIRM_KEY] = DEFAULT_RAFT_NEED_MAJORITY_CONFIRM ? "true" : "false";
    }

    /**
     * @brief 验证所有关键配置参数的有效性
     * @details 提取冗余的 if 判断为 lambda 通用验证器
     */
    void check_config()
    {
        // 通用数值范围验证器
        auto check_int = [&](const char *key, int min_val, int max_val = INT32_MAX)
        {
            if (auto it = config_map_.find(key); it != config_map_.end())
            {
                try
                {
                    int val = std::stoi(it->second);
                    if (val < min_val || val > max_val)
                    {
                        throw std::runtime_error("Invalid range for " + std::string(key) + ": " + it->second);
                    }
                }
                catch (const std::exception &)
                {
                    throw std::runtime_error("Invalid integer format for " + std::string(key) + ": " + it->second);
                }
            }
        };

        // 通用布尔型验证器
        auto check_bool = [&](const char *key)
        {
            if (auto it = config_map_.find(key); it != config_map_.end())
            {
                const auto &v = it->second;
                if (v != "true" && v != "false" && v != "0" && v != "1")
                {
                    throw std::runtime_error("Invalid boolean format for " + std::string(key) + ": " + v);
                }
            }
        };

        // 基础设定检查
        check_int(LOG_LEVEL_KEY, 0, 4);
        check_int(LOG_ROTATE_SIZE_KEY, 1);
        check_int(PORT_KEY, 1024, 65535);
        check_bool(READ_ONLY_KEY);

        // 数据结构和存储检查
        check_int(SKIPLIST_MAX_LEVEL_KEY, 1, 64);
        if (auto it = config_map_.find(SKIPLIST_PROBABILITY_KEY); it != config_map_.end())
        {
            double prob = std::stod(it->second);
            if (prob <= 0.0 || prob >= 1.0)
            {
                throw std::runtime_error("Invalid skiplist_probability: " + it->second);
            }
        }
        check_int(SKIPLIST_MAX_NODE_COUNT_KEY, 1);
        check_int(MEMTABLE_SIZE_KEY, 1);

        // WAL 和 SSTable 检查
        check_bool(WAL_ENABLE_KEY);
        check_int(WAL_FILE_SIZE_KEY, 1);
        check_int(WAL_FILE_MAX_COUNT_KEY, 1);
        check_int(WAL_FLUSH_STRATEGY_KEY, 0, 2);
        check_int(WAL_FLUSH_INTERVAL_KEY, 1);
        check_int(SSTABLE_MERGE_STRATEGY_KEY, 0, 1);
        check_int(SSTABLE_MERGE_THRESHOLD_KEY, 1);
        check_int(SSTABLE_ZERO_LEVEL_SIZE_KEY, 1);
        check_int(SSTABLE_LEVEL_SIZE_RATIO_KEY, 2); // > 1

        // 并发及网络检查
        check_int(MAX_CONNECTIONS_KEY, 1, 100000);
        check_int(MEMORY_POOL_SIZE_KEY, 1);
        check_int(WAITING_QUEUE_SIZE_KEY, 1);
        check_int(MAX_WAITING_TIME_KEY, 1);
        check_int(WORKER_THREAD_COUNT_KEY, 1);
        check_int(WORKER_QUEUE_SIZE_KEY, 1);
        check_int(WORKER_WAIT_TIMEOUT_KEY, 1);
    }
};

#endif // CONFIG_H