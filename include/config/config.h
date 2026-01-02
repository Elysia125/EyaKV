#ifndef CONFIG_H
#define CONFIG_H
#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <optional>
#include "common/path_utils.h"

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
#define DEFAULT_READ_ONLY false
#define DEFAULT_LOG_LEVEL LogLevel::INFO
#define DEFAULT_LOG_ROTATE_SIZE 1024 * 5 // 日志轮转阈值，避免日志文件过大
#define DEFAULT_SKIPLIST_MAX_LEVEL 16
#define DEFAULT_SKIPLIST_PROBABILITY 0.5
#define DEFAULT_SKIPLIST_MAX_NODE_COUNT 10000000
#define DEFAULT_MEMTABLE_SIZE 1024 * 1024 * 1024 // kb
#define DEFAULT_WAL_ENABLE true
#define DEFAULT_WAL_FILE_SIZE 1024 * 1024 * 1024 // kb
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

#define PORT_KEY "port"
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
class EyaKVConfig
{
private:
    std::string config_file_ = PathUtils::GetTargetFilePath("conf/eyakv.conf");
    std::unordered_map<std::string, std::string> config_map_;
    EyaKVConfig()
    {
        load_default_config();
        load_config();
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

    void load_default_config()
    {
        config_map_[LOG_DIR_KEY] = PathUtils::GetTargetFilePath("logs");
        config_map_[LOG_LEVEL_KEY] = std::to_string(static_cast<int>(DEFAULT_LOG_LEVEL));
        config_map_[LOG_ROTATE_SIZE_KEY] = std::to_string(DEFAULT_LOG_ROTATE_SIZE);
        config_map_[PORT_KEY] = std::to_string(DEFAULT_PORT);
        config_map_[READ_ONLY_KEY] = std::to_string(DEFAULT_READ_ONLY);
        config_map_[SKIPLIST_MAX_LEVEL_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_LEVEL);
        config_map_[SKIPLIST_PROBABILITY_KEY] = std::to_string(DEFAULT_SKIPLIST_PROBABILITY);
        config_map_[SKIPLIST_MAX_NODE_COUNT_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_NODE_COUNT);
        config_map_[MEMTABLE_SIZE_KEY] = std::to_string(DEFAULT_MEMTABLE_SIZE);
        config_map_[WAL_ENABLE_KEY] = std::to_string(DEFAULT_WAL_ENABLE);
        config_map_[WAL_DIR_KEY] = PathUtils::CombinePath(PathUtils::GetTargetFilePath("data"), "wal");
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
        config_map_[DATA_DIR_KEY] = PathUtils::GetTargetFilePath("data");
        config_map_[PASSWORD_KEY] = "";
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
#endif