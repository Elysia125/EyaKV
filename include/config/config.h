#ifndef CONFIG_H
#define CONFIG_H
#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <optional>
#include <thread>
#include "common/util/path_utils.h"

#undef ERROR // 避免与 LogLevel 枚举冲突

// 日志级别枚举
enum class LogLevel
{
    DEBUG = 0, // 调试信息
    INFO,      // 普通信息
    WARN,      // 警告
    ERROR,     // 错误
    FATAL      // 致命错误
};
enum class WALFlushStrategy
{
    BACKGROUND_THREAD = 0, // 后台线程定时刷新
    IMMEDIATE_ON_WRITE,    // 写入时立即刷新
    OS_BUFFERED            // 写入到内核缓冲区，依靠操作系统刷新
};
enum class SSTableMergeStrategy
{
    SIZE_TIERED_COMPACTION = 0, // 大小分层压缩
    LEVEL_COMPACTION,           // 分层合并
};
#define DEFAULT_PORT 5210
#define DEFAULT_IP "0.0.0.0"
#define DEFAULT_RAFT_PORT 5211
#define DEFAULT_RAFT_TRUST_IP "127.0.0.1"
#define DEFAULT_READ_ONLY false
#define DEFAULT_LOG_LEVEL LogLevel::INFO
#define DEFAULT_LOG_ROTATE_SIZE 1024 * 1024 // KB 日志轮转阈值，避免日志文件过大
#define DEFAULT_SKIPLIST_MAX_LEVEL 16
#define DEFAULT_SKIPLIST_PROBABILITY 0.5
#define DEFAULT_SKIPLIST_MAX_NODE_COUNT 10000000
#define DEFAULT_MEMTABLE_SIZE 1024 * 1024 // kb
#define DEFAULT_WAL_ENABLE true
#define DEFAULT_WAL_FILE_SIZE 1024 * 1024 * 10 // kb
#define DEFAULT_WAL_FILE_MAX_COUNT 10
#define DEFAULT_WAL_FLUSH_INTERVAL 1000                                             // ms wal刷新间隔
#define DEFAULT_WAL_FLUSH_STRATEGY WALFlushStrategy::BACKGROUND_THREAD              // wal刷新策略
#define DEFAULT_SSTABLE_MERGE_STRATEGY SSTableMergeStrategy::SIZE_TIERED_COMPACTION // sstable 合并策略
#define DEFAULT_SSTABLE_MERGE_THRESHOLD 5                                           // sstable 合并阈值，文件数 对大小分层合并策略有效
#define DEFAULT_SSTABLE_ZERO_LEVEL_SIZE 10                                          // sstable 0 层大小（MB） 对分层合并策略有效
#define DEFAULT_SSTABLE_LEVEL_SIZE_RATIO 10                                         // sstable 层大小比例 对分层合并策略有效                                               // wal刷新策略 0: 后台线程每隔一段时间刷 1: 写入时立刻刷 2:写入到内核缓冲区，依靠操作系统刷
#define DEFAULT_MAX_CONNECTIONS 10000                                               // 最大连接数
#define DEFAULT_MEMORY_POOL_SIZE 1024 * 3                                           // 内存池大小
#define DEFAULT_WAITING_QUEUE_SIZE 100                                              //  等待队列大小
#define DEFAULT_MAX_WAITING_TIME 30                                                 // 最大等待时间 s
#define DEFAULT_WORKER_THREAD_COUNT std::thread::hardware_concurrency() + 1
#define DEFAULT_WORKER_QUEUE_SIZE 1000
#define DEFAULT_WORKER_WAIT_TIMEOUT 30

// Raft 相关默认配置
#define DEFAULT_RAFT_ELECTION_TIMEOUT_MIN 150    // 选举超时最小值(ms)
#define DEFAULT_RAFT_ELECTION_TIMEOUT_MAX 300    // 选举超时最大值(ms)
#define DEFAULT_RAFT_HEARTBEAT_INTERVAL 30       // 心跳间隔(ms)
#define DEFAULT_RAFT_RPC_TIMEOUT 2000            // Raft RPC 超时(ms)
#define DEFAULT_RAFT_FOLLOWER_IDLE_WAIT 1000     // Follower 空闲等待(ms)
#define DEFAULT_RAFT_JOIN_MAX_RETRIES 3          // Follower 加入集群最大重试次数
#define DEFAULT_RAFT_REQUEST_VOTE_TIMEOUT 200    // RequestVote 响应超时(ms)
#define DEFAULT_RAFT_SUBMIT_TIMEOUT 2000         // 提交命令等待超时(ms)
#define DEFAULT_RAFT_APPEND_BATCH 100            // 单次 AppendEntries 最大日志条数
#define DEFAULT_RAFT_SNAPSHOT_CHUNK (64 * 1024)  // 快照 chunk 大小(bytes)
#define DEFAULT_RAFT_RESULT_CACHE_CAPACITY 10000 // 结果缓存容量
#define DEFAULT_RAFT_THREADPOOL_WORKERS 4        // Raft 内部线程池工作线程数
#define DEFAULT_RAFT_THREADPOOL_QUEUE 10000      // Raft 内部线程池队列大小
#define DEFAULT_RAFT_THREADPOOL_WAIT 1000        // Raft 内部线程池等待超时(ms)

#define DEFAULT_RAFT_LOG_THRESHOLD 1000000           // 触发日志截断的阈值(条数)
#define DEFAULT_RAFT_LOG_TRUNCATE_RATIO 0.25         // 截断比例
#define DEFAULT_RAFT_WAL_FILENAME "raft_wal.log"     // WAL 文件名
#define DEFAULT_RAFT_INDEX_FILENAME "raft_index.idx" // 索引文件名

#define PORT_KEY "port"
#define IP_KEY "ip"
#define RAFT_PORT_KEY "raft_port"
#define RAFT_TRUST_IP_KEY "raft_trust_ip"
#define READ_ONLY_KEY "read_only"
#define LOG_LEVEL_KEY "log_level"
#define LOG_ROTATE_SIZE_KEY "log_rotate_size"
#define SKIPLIST_MAX_LEVEL_KEY "skiplist_max_level"
#define SKIPLIST_PROBABILITY_KEY "skiplist_probability"
#define SKIPLIST_MAX_NODE_COUNT_KEY "skiplist_max_node_count"
#define MEMTABLE_SIZE_KEY "memtable_size"
#define WAL_ENABLE_KEY "wal_enable"
#define WAL_DIR_KEY "wal_dir"
#define WAL_FILE_SIZE_KEY "wal_file_size"
#define WAL_FILE_MAX_COUNT_KEY "wal_file_max_count"
#define WAL_FLUSH_INTERVAL_KEY "wal_flush_interval"
#define WAL_FLUSH_STRATEGY_KEY "wal_flush_strategy"
#define SSTABLE_MERGE_STRATEGY_KEY "sstable_merge_strategy"
#define SSTABLE_ZERO_LEVEL_SIZE_KEY "sstable_zero_level_size"
#define SSTABLE_LEVEL_SIZE_RATIO_KEY "sstable_level_size_ratio"
#define SSTABLE_MERGE_THRESHOLD_KEY "sstable_merge_threshold"
#define MAX_CONNECTIONS_KEY "max_connections"
#define MEMORY_POOL_SIZE_KEY "memory_pool_size"
#define WAITING_QUEUE_SIZE_KEY "waiting_queue_size"
#define MAX_WAITING_TIME_KEY "max_waiting_time"
#define LOG_DIR_KEY "log_dir"
#define DATA_DIR_KEY "data_dir"
#define PASSWORD_KEY "password"
#define WORKER_THREAD_COUNT_KEY "worker_thread_count"
#define WORKER_QUEUE_SIZE_KEY "worker_queue_size"
#define WORKER_WAIT_TIMEOUT_KEY "worker_wait_timeout"

// Raft 相关配置 key
#define RAFT_ELECTION_TIMEOUT_MIN_KEY "raft_election_timeout_min_ms"
#define RAFT_ELECTION_TIMEOUT_MAX_KEY "raft_election_timeout_max_ms"
#define RAFT_HEARTBEAT_INTERVAL_KEY "raft_heartbeat_interval_ms"
#define RAFT_RPC_TIMEOUT_KEY "raft_rpc_timeout_ms"
#define RAFT_FOLLOWER_IDLE_WAIT_KEY "raft_follower_idle_wait_ms"
#define RAFT_JOIN_MAX_RETRIES_KEY "raft_join_max_retries"
#define RAFT_REQUEST_VOTE_TIMEOUT_KEY "raft_request_vote_timeout_ms"
#define RAFT_SUBMIT_TIMEOUT_KEY "raft_submit_timeout_ms"
#define RAFT_APPEND_BATCH_KEY "raft_append_entries_max_batch"
#define RAFT_SNAPSHOT_CHUNK_KEY "raft_snapshot_chunk_size_bytes"
#define RAFT_RESULT_CACHE_CAPACITY_KEY "raft_result_cache_capacity"
#define RAFT_THREADPOOL_WORKERS_KEY "raft_threadpool_workers"
#define RAFT_THREADPOOL_QUEUE_KEY "raft_threadpool_queue_size"
#define RAFT_THREADPOOL_WAIT_KEY "raft_threadpool_wait_timeout_ms"

#define RAFT_LOG_THRESHOLD_KEY "raft_log_size_threshold"
#define RAFT_LOG_TRUNCATE_RATIO_KEY "raft_log_truncate_ratio"
#define RAFT_WAL_FILENAME_KEY "raft_wal_filename"
#define RAFT_INDEX_FILENAME_KEY "raft_index_filename"

class EyaKVConfig
{
private:
    std::string config_file_;
    std::unordered_map<std::string, std::string> config_map_;

    // 配置 key 到环境变量 key 的映射表
    static const std::unordered_map<std::string, std::string> ENV_KEY_MAP;

    // 初始化环境变量映射表
    static std::unordered_map<std::string, std::string> init_env_key_map();

    EyaKVConfig()
    {
        const char *env_config = std::getenv("EYAKV_CONFIG_PATH");
        if (env_config != nullptr)
        {
            config_file_ = std::string(env_config);
        }
        else
        {
            config_file_ = PathUtils::get_target_file_path("conf/eyakv.conf");
        }
        load_default_config();
        load_config();
        load_config_from_env(); // 从环境变量加载配置
        check_config();
    }
    void load_config()
    {
        if (!std::filesystem::exists(config_file_))
        {
            return;
        }
        std::ifstream config_file(config_file_);
        if (!config_file.is_open())
        {
            throw std::runtime_error("Failed to open config file: " + config_file_);
        }
        std::string line;
        while (std::getline(config_file, line))
        {
            size_t index = line.find('#');
            if (index != std::string::npos)
            {
                line = line.substr(0, index);
            }
            if (line.empty())
            {
                continue;
            }
            index = line.find('=');
            if (index == std::string::npos)
            {
                throw std::runtime_error("Invalid config line: " + line);
            }
            std::string key = trim(line.substr(0, index)), value = trim(line.substr(index + 1));
            if (key.empty() || value.empty())
            {
                throw std::runtime_error("Invalid config line: " + line);
            }
            config_map_[key] = value;
        }
    }

    /**
     * @brief 从环境变量加载配置
     *
     * 环境变量的命名规则：EYAKV_<KEY_NAME>
     * 例如：port -> EYAKV_PORT, log_level -> EYAKV_LOG_LEVEL
     * 环境变量优先级高于配置文件
     */
    void load_config_from_env()
    {
        for (const auto &[config_key, env_key] : ENV_KEY_MAP)
        {
            const char *env_value = std::getenv(env_key.c_str());
            if (env_value != nullptr && std::string(env_value).length() > 0)
            {
                config_map_[config_key] = std::string(env_value);
            }
        }
    }

    void load_default_config()
    {
        config_map_[LOG_DIR_KEY] = PathUtils::get_target_file_path("logs");
        config_map_[LOG_LEVEL_KEY] = std::to_string(static_cast<int>(DEFAULT_LOG_LEVEL));
        config_map_[LOG_ROTATE_SIZE_KEY] = std::to_string(DEFAULT_LOG_ROTATE_SIZE);
        config_map_[IP_KEY] = DEFAULT_IP;
        config_map_[PORT_KEY] = std::to_string(DEFAULT_PORT);
        config_map_[RAFT_PORT_KEY] = std::to_string(DEFAULT_RAFT_PORT);
        config_map_[RAFT_TRUST_IP_KEY] = DEFAULT_RAFT_TRUST_IP;
        config_map_[READ_ONLY_KEY] = std::to_string(DEFAULT_READ_ONLY);
        config_map_[SKIPLIST_MAX_LEVEL_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_LEVEL);
        config_map_[SKIPLIST_PROBABILITY_KEY] = std::to_string(DEFAULT_SKIPLIST_PROBABILITY);
        config_map_[SKIPLIST_MAX_NODE_COUNT_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_NODE_COUNT);
        config_map_[MEMTABLE_SIZE_KEY] = std::to_string(DEFAULT_MEMTABLE_SIZE);
        config_map_[WAL_ENABLE_KEY] = std::to_string(DEFAULT_WAL_ENABLE);
        config_map_[WAL_DIR_KEY] = PathUtils::combine_path(PathUtils::get_target_file_path("data"), "wal");
        config_map_[WAL_FILE_SIZE_KEY] = std::to_string(DEFAULT_WAL_FILE_SIZE);
        config_map_[WAL_FILE_MAX_COUNT_KEY] = std::to_string(DEFAULT_WAL_FILE_MAX_COUNT);
        config_map_[SSTABLE_MERGE_THRESHOLD_KEY] = std::to_string(DEFAULT_SSTABLE_MERGE_THRESHOLD);
        config_map_[SSTABLE_MERGE_STRATEGY_KEY] = std::to_string(static_cast<int>(DEFAULT_SSTABLE_MERGE_STRATEGY));
        config_map_[SSTABLE_ZERO_LEVEL_SIZE_KEY] = std::to_string(DEFAULT_SSTABLE_ZERO_LEVEL_SIZE);
        config_map_[SSTABLE_LEVEL_SIZE_RATIO_KEY] = std::to_string(DEFAULT_SSTABLE_LEVEL_SIZE_RATIO);
        config_map_[WAL_FLUSH_STRATEGY_KEY] = std::to_string(static_cast<int>(DEFAULT_WAL_FLUSH_STRATEGY));
        config_map_[WAL_FLUSH_INTERVAL_KEY] = std::to_string(DEFAULT_WAL_FLUSH_INTERVAL);
        config_map_[MAX_CONNECTIONS_KEY] = std::to_string(DEFAULT_MAX_CONNECTIONS);
        config_map_[MEMORY_POOL_SIZE_KEY] = std::to_string(DEFAULT_MEMORY_POOL_SIZE);
        config_map_[WAITING_QUEUE_SIZE_KEY] = std::to_string(DEFAULT_WAITING_QUEUE_SIZE);
        config_map_[MAX_WAITING_TIME_KEY] = std::to_string(DEFAULT_MAX_WAITING_TIME);
        config_map_[DATA_DIR_KEY] = PathUtils::get_target_file_path("data");
        config_map_[PASSWORD_KEY] = "";
        config_map_[WORKER_THREAD_COUNT_KEY] = std::to_string(DEFAULT_WORKER_THREAD_COUNT);
        config_map_[WORKER_QUEUE_SIZE_KEY] = std::to_string(DEFAULT_WORKER_QUEUE_SIZE);
        config_map_[WORKER_WAIT_TIMEOUT_KEY] = std::to_string(DEFAULT_WORKER_WAIT_TIMEOUT);

        // Raft 相关默认配置
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
    }

    void check_config()
    {
        if (config_map_.find(LOG_LEVEL_KEY) != config_map_.end())
        {
            int log_level = std::stoi(config_map_[LOG_LEVEL_KEY]);
            if (log_level < 0 || log_level > 4)
            {
                throw std::runtime_error("Invalid log level: " + config_map_[LOG_LEVEL_KEY]);
            }
        }
        if (config_map_.find(LOG_ROTATE_SIZE_KEY) != config_map_.end())
        {
            int log_rotate_size = std::stoi(config_map_[LOG_ROTATE_SIZE_KEY]);
            if (log_rotate_size <= 0)
            {
                throw std::runtime_error("Invalid log rotate size: " + config_map_[LOG_ROTATE_SIZE_KEY]);
            }
        }
        if (config_map_.find(PORT_KEY) != config_map_.end())
        {
            int port = std::stoi(config_map_[PORT_KEY]);
            if (port < 1024 || port > 65535)
            {
                throw std::runtime_error("Invalid port: " + config_map_[PORT_KEY]);
            }
        }
        if (config_map_.find(READ_ONLY_KEY) != config_map_.end())
        {
            std::string read_only = config_map_[READ_ONLY_KEY];
            if (read_only != "true" && read_only != "false" && read_only != "0" && read_only != "1")
            {
                throw std::runtime_error("Invalid read_only: " + config_map_[READ_ONLY_KEY]);
            }
        }

        // Skiplist配置校验
        if (config_map_.find(SKIPLIST_MAX_LEVEL_KEY) != config_map_.end())
        {
            int max_level = std::stoi(config_map_[SKIPLIST_MAX_LEVEL_KEY]);
            if (max_level <= 0 || max_level > 64)
            {
                throw std::runtime_error("Invalid skiplist_max_level: " + config_map_[SKIPLIST_MAX_LEVEL_KEY]);
            }
        }
        if (config_map_.find(SKIPLIST_PROBABILITY_KEY) != config_map_.end())
        {
            double probability = std::stod(config_map_[SKIPLIST_PROBABILITY_KEY]);
            if (probability <= 0.0 || probability >= 1.0)
            {
                throw std::runtime_error("Invalid skiplist_probability: " + config_map_[SKIPLIST_PROBABILITY_KEY]);
            }
        }
        if (config_map_.find(SKIPLIST_MAX_NODE_COUNT_KEY) != config_map_.end())
        {
            int max_node_count = std::stoi(config_map_[SKIPLIST_MAX_NODE_COUNT_KEY]);
            if (max_node_count <= 0)
            {
                throw std::runtime_error("Invalid skiplist_max_node_count: " + config_map_[SKIPLIST_MAX_NODE_COUNT_KEY]);
            }
        }

        // MemTable配置校验
        if (config_map_.find(MEMTABLE_SIZE_KEY) != config_map_.end())
        {
            int memtable_size = std::stoi(config_map_[MEMTABLE_SIZE_KEY]);
            if (memtable_size <= 0)
            {
                throw std::runtime_error("Invalid memtable_size: " + config_map_[MEMTABLE_SIZE_KEY]);
            }
        }

        // WAL配置校验
        if (config_map_.find(WAL_ENABLE_KEY) != config_map_.end())
        {
            std::string wal_enable = config_map_[WAL_ENABLE_KEY];
            if (wal_enable != "true" && wal_enable != "false" && wal_enable != "0" && wal_enable != "1")
            {
                throw std::runtime_error("Invalid wal_enable: " + config_map_[WAL_ENABLE_KEY]);
            }
        }
        if (config_map_.find(WAL_FILE_SIZE_KEY) != config_map_.end())
        {
            int wal_file_size = std::stoi(config_map_[WAL_FILE_SIZE_KEY]);
            if (wal_file_size <= 0)
            {
                throw std::runtime_error("Invalid wal_file_size: " + config_map_[WAL_FILE_SIZE_KEY]);
            }
        }
        if (config_map_.find(WAL_FILE_MAX_COUNT_KEY) != config_map_.end())
        {
            int wal_file_max_count = std::stoi(config_map_[WAL_FILE_MAX_COUNT_KEY]);
            if (wal_file_max_count <= 0)
            {
                throw std::runtime_error("Invalid wal_file_max_count: " + config_map_[WAL_FILE_MAX_COUNT_KEY]);
            }
        }

        // WAL刷新策略校验
        if (config_map_.find(WAL_FLUSH_STRATEGY_KEY) != config_map_.end())
        {
            int wal_flush_strategy = std::stoi(config_map_[WAL_FLUSH_STRATEGY_KEY]);
            if (wal_flush_strategy < 0 || wal_flush_strategy > 2)
            {
                throw std::runtime_error("Invalid wal_flush_strategy: " + config_map_[WAL_FLUSH_STRATEGY_KEY]);
            }
        }
        if (config_map_.find(WAL_FLUSH_INTERVAL_KEY) != config_map_.end())
        {
            int wal_flush_interval = std::stoi(config_map_[WAL_FLUSH_INTERVAL_KEY]);
            if (wal_flush_interval <= 0)
            {
                throw std::runtime_error("Invalid wal_flush_interval: " + config_map_[WAL_FLUSH_INTERVAL_KEY]);
            }
        }

        // SSTable配置校验
        if (config_map_.find(SSTABLE_MERGE_STRATEGY_KEY) != config_map_.end())
        {
            int merge_strategy = std::stoi(config_map_[SSTABLE_MERGE_STRATEGY_KEY]);
            if (merge_strategy < 0 || merge_strategy > 1)
            {
                throw std::runtime_error("Invalid sstable_merge_strategy: " + config_map_[SSTABLE_MERGE_STRATEGY_KEY]);
            }
        }
        if (config_map_.find(SSTABLE_MERGE_THRESHOLD_KEY) != config_map_.end())
        {
            int merge_threshold = std::stoi(config_map_[SSTABLE_MERGE_THRESHOLD_KEY]);
            if (merge_threshold <= 0)
            {
                throw std::runtime_error("Invalid sstable_merge_threshold: " + config_map_[SSTABLE_MERGE_THRESHOLD_KEY]);
            }
        }
        if (config_map_.find(SSTABLE_ZERO_LEVEL_SIZE_KEY) != config_map_.end())
        {
            int zero_level_size = std::stoi(config_map_[SSTABLE_ZERO_LEVEL_SIZE_KEY]);
            if (zero_level_size <= 0)
            {
                throw std::runtime_error("Invalid sstable_zero_level_size: " + config_map_[SSTABLE_ZERO_LEVEL_SIZE_KEY]);
            }
        }
        if (config_map_.find(SSTABLE_LEVEL_SIZE_RATIO_KEY) != config_map_.end())
        {
            int level_size_ratio = std::stoi(config_map_[SSTABLE_LEVEL_SIZE_RATIO_KEY]);
            if (level_size_ratio <= 1)
            {
                throw std::runtime_error("Invalid sstable_level_size_ratio: " + config_map_[SSTABLE_LEVEL_SIZE_RATIO_KEY]);
            }
        }

        // 连接和内存池配置校验
        if (config_map_.find(MAX_CONNECTIONS_KEY) != config_map_.end())
        {
            int max_connections = std::stoi(config_map_[MAX_CONNECTIONS_KEY]);
            if (max_connections <= 0 || max_connections > 100000)
            {
                throw std::runtime_error("Invalid max_connections: " + config_map_[MAX_CONNECTIONS_KEY]);
            }
        }
        if (config_map_.find(MEMORY_POOL_SIZE_KEY) != config_map_.end())
        {
            int memory_pool_size = std::stoi(config_map_[MEMORY_POOL_SIZE_KEY]);
            if (memory_pool_size <= 0)
            {
                throw std::runtime_error("Invalid memory_pool_size: " + config_map_[MEMORY_POOL_SIZE_KEY]);
            }
        }
        if (config_map_.find(WAITING_QUEUE_SIZE_KEY) != config_map_.end())
        {
            int waiting_queue_size = std::stoi(config_map_[WAITING_QUEUE_SIZE_KEY]);
            if (waiting_queue_size <= 0)
            {
                throw std::runtime_error("Invalid waiting_queue_size: " + config_map_[WAITING_QUEUE_SIZE_KEY]);
            }
        }
        if (config_map_.find(MAX_WAITING_TIME_KEY) != config_map_.end())
        {
            int max_waiting_time = std::stoi(config_map_[MAX_WAITING_TIME_KEY]);
            if (max_waiting_time <= 0)
            {
                throw std::runtime_error("Invalid max_waiting_time: " + config_map_[MAX_WAITING_TIME_KEY]);
            }
        }
        // 工作线程配置校验
        if (config_map_.find(WORKER_THREAD_COUNT_KEY) != config_map_.end())
        {
            int worker_thread_num = std::stoi(config_map_[WORKER_THREAD_COUNT_KEY]);
            if (worker_thread_num <= 0)
            {
                throw std::runtime_error("Invalid worker_thread_num: " + config_map_[WORKER_THREAD_COUNT_KEY]);
            }
        }
        if (config_map_.find(WORKER_QUEUE_SIZE_KEY) != config_map_.end())
        {
            int worker_queue_size = std::stoi(config_map_[WORKER_QUEUE_SIZE_KEY]);
            if (worker_queue_size <= 0)
            {
                throw std::runtime_error("Invalid worker_queue_size: " + config_map_[WORKER_QUEUE_SIZE_KEY]);
            }
        }
        if (config_map_.find(WORKER_WAIT_TIMEOUT_KEY) != config_map_.end())
        {
            int worker_wait_timeout = std::stoi(config_map_[WORKER_WAIT_TIMEOUT_KEY]);
            if (worker_wait_timeout <= 0)
            {
                throw std::runtime_error("Invalid worker_wait_timeout: " + config_map_[WORKER_WAIT_TIMEOUT_KEY]);
            }
        }
    }

    std::string trim(const std::string &str)
    {
        size_t start = 0, end = str.size() - 1;
        while (start < str.size() && isspace(str[start]))
        {
            start++;
        }
        while (end > 0 && isspace(str[end]))
        {
            end--;
        }
        return str.substr(start, end - start + 1);
    }

public:
    static EyaKVConfig &get_instance()
    {
        static EyaKVConfig instance;
        return instance;
    }
    std::optional<std::string> get_config(const std::string &key) const
    {

        if (config_map_.find(key) != config_map_.end())
        {
            return config_map_.at(key);
        }
        return std::nullopt;
    }
    ~EyaKVConfig() = default;
};

// 初始化环境变量映射表的静态实现
inline std::unordered_map<std::string, std::string> EyaKVConfig::init_env_key_map()
{
    return {
        // 网络配置
        {PORT_KEY, "EYAKV_PORT"},
        {IP_KEY, "EYAKV_IP"},
        {RAFT_PORT_KEY, "EYAKV_RAFT_PORT"},
        {RAFT_TRUST_IP_KEY, "EYAKV_RAFT_TRUST_IP"},

        // 日志配置
        {LOG_LEVEL_KEY, "EYAKV_LOG_LEVEL"},
        {LOG_ROTATE_SIZE_KEY, "EYAKV_LOG_ROTATE_SIZE"},
        {LOG_DIR_KEY, "EYAKV_LOG_DIR"},

        // 存储配置
        {READ_ONLY_KEY, "EYAKV_READ_ONLY"},
        {MEMTABLE_SIZE_KEY, "EYAKV_MEMTABLE_SIZE"},
        {DATA_DIR_KEY, "EYAKV_DATA_DIR"},

        // SkipList 配置
        {SKIPLIST_MAX_LEVEL_KEY, "EYAKV_SKIPLIST_MAX_LEVEL"},
        {SKIPLIST_PROBABILITY_KEY, "EYAKV_SKIPLIST_PROBABILITY"},
        {SKIPLIST_MAX_NODE_COUNT_KEY, "EYAKV_SKIPLIST_MAX_NODE_COUNT"},

        // WAL 配置
        {WAL_ENABLE_KEY, "EYAKV_WAL_ENABLE"},
        {WAL_DIR_KEY, "EYAKV_WAL_DIR"},
        {WAL_FILE_SIZE_KEY, "EYAKV_WAL_FILE_SIZE"},
        {WAL_FILE_MAX_COUNT_KEY, "EYAKV_WAL_FILE_MAX_COUNT"},
        {WAL_FLUSH_INTERVAL_KEY, "EYAKV_WAL_FLUSH_INTERVAL"},
        {WAL_FLUSH_STRATEGY_KEY, "EYAKV_WAL_FLUSH_STRATEGY"},

        // SSTable 配置
        {SSTABLE_MERGE_STRATEGY_KEY, "EYAKV_SSTABLE_MERGE_STRATEGY"},
        {SSTABLE_ZERO_LEVEL_SIZE_KEY, "EYAKV_SSTABLE_ZERO_LEVEL_SIZE"},
        {SSTABLE_LEVEL_SIZE_RATIO_KEY, "EYAKV_SSTABLE_LEVEL_SIZE_RATIO"},
        {SSTABLE_MERGE_THRESHOLD_KEY, "EYAKV_SSTABLE_MERGE_THRESHOLD"},

        // 连接和线程配置
        {MAX_CONNECTIONS_KEY, "EYAKV_MAX_CONNECTIONS"},
        {MEMORY_POOL_SIZE_KEY, "EYAKV_MEMORY_POOL_SIZE"},
        {WAITING_QUEUE_SIZE_KEY, "EYAKV_WAITING_QUEUE_SIZE"},
        {MAX_WAITING_TIME_KEY, "EYAKV_MAX_WAITING_TIME"},
        {PASSWORD_KEY, "EYAKV_PASSWORD"},
        {WORKER_THREAD_COUNT_KEY, "EYAKV_WORKER_THREAD_COUNT"},
        {WORKER_QUEUE_SIZE_KEY, "EYAKV_WORKER_QUEUE_SIZE"},
        {WORKER_WAIT_TIMEOUT_KEY, "EYAKV_WORKER_WAIT_TIMEOUT"},

        // Raft 选举配置
        {RAFT_ELECTION_TIMEOUT_MIN_KEY, "EYAKV_RAFT_ELECTION_TIMEOUT_MIN_MS"},
        {RAFT_ELECTION_TIMEOUT_MAX_KEY, "EYAKV_RAFT_ELECTION_TIMEOUT_MAX_MS"},
        {RAFT_HEARTBEAT_INTERVAL_KEY, "EYAKV_RAFT_HEARTBEAT_INTERVAL_MS"},
        {RAFT_RPC_TIMEOUT_KEY, "EYAKV_RAFT_RPC_TIMEOUT_MS"},
        {RAFT_FOLLOWER_IDLE_WAIT_KEY, "EYAKV_RAFT_FOLLOWER_IDLE_WAIT_MS"},
        {RAFT_JOIN_MAX_RETRIES_KEY, "EYAKV_RAFT_JOIN_MAX_RETRIES"},

        // Raft 日志配置
        {RAFT_REQUEST_VOTE_TIMEOUT_KEY, "EYAKV_RAFT_REQUEST_VOTE_TIMEOUT_MS"},
        {RAFT_SUBMIT_TIMEOUT_KEY, "EYAKV_RAFT_SUBMIT_TIMEOUT_MS"},
        {RAFT_APPEND_BATCH_KEY, "EYAKV_RAFT_APPEND_ENTRIES_MAX_BATCH"},
        {RAFT_LOG_THRESHOLD_KEY, "EYAKV_RAFT_LOG_SIZE_THRESHOLD"},
        {RAFT_LOG_TRUNCATE_RATIO_KEY, "EYAKV_RAFT_LOG_TRUNCATE_RATIO"},
        {RAFT_WAL_FILENAME_KEY, "EYAKV_RAFT_WAL_FILENAME"},
        {RAFT_INDEX_FILENAME_KEY, "EYAKV_RAFT_INDEX_FILENAME"},

        // Raft 快照配置
        {RAFT_SNAPSHOT_CHUNK_KEY, "EYAKV_RAFT_SNAPSHOT_CHUNK_SIZE_BYTES"},
        {RAFT_RESULT_CACHE_CAPACITY_KEY, "EYAKV_RAFT_RESULT_CACHE_CAPACITY"},

        // Raft 线程池配置
        {RAFT_THREADPOOL_WORKERS_KEY, "EYAKV_RAFT_THREADPOOL_WORKERS"},
        {RAFT_THREADPOOL_QUEUE_KEY, "EYAKV_RAFT_THREADPOOL_QUEUE_SIZE"},
        {RAFT_THREADPOOL_WAIT_KEY, "EYAKV_RAFT_THREADPOOL_WAIT_TIMEOUT_MS"},
    };
}

// 静态成员变量定义
inline const std::unordered_map<std::string, std::string> EyaKVConfig::ENV_KEY_MAP = EyaKVConfig::init_env_key_map();
#endif