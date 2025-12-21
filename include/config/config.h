#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <optional>
#include "common/path_utils.h"
// 日志级别枚举
enum class LogLevel
{
    DEBUG = 0, // 调试信息
    INFO,      // 普通信息
    WARN,      // 警告
    ERROR,     // 错误
    FATAL      // 致命错误
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
#define DEFAULT_WAL_SYNC_INTERVAL 1000    // ms
#define DEFAULT_SSTABLE_MERGE_THRESHOLD 5 // sstable 合并阈值，文件数
#define DEFAULT_DATA_FULSH_INTERVAL 1000  // ms 数据刷新间隔
#define DEFAULT_DATA_FLUSH_STRATEGY 0     // 数据刷新策略 0: 后台线程每隔一段时间刷 1: 写入时立刻刷 2:写入到内核缓冲区，依靠操作系统刷
#define DEFAULT_MAX_CONNECTIONS 10000     // 最大连接数
#define DEFAULT_MEMORY_POOL_SIZE 1024 * 3 // 内存池大小
#define DEFAULT_WAITING_QUEUE_SIZE 100    //  等待队列大小
#define DEFAULT_MAX_WAITING_TIME 30       // 最大等待时间 s

const std::string DEFAULT_WAL_DIR = PathUtils::GetTargetFilePath("data/wal");
const std::string DEFAULT_LOG_DIR = PathUtils::GetTargetFilePath("logs");
const std::string DEFAULT_DATA_DIR = PathUtils::GetTargetFilePath("data");

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
#define WAL_SYNC_INTERVAL_KEY "wal_sync_interval"
#define SSTABLE_MERGE_THRESHOLD_KEY "sstable_merge_threshold"
#define DATA_FLUSH_INTERVAL_KEY "data_flush_interval"
#define DATA_FLUSH_STRATEGY_KEY "data_flush_strategy"
#define MAX_CONNECTIONS_KEY "max_connections"
#define MEMORY_POOL_SIZE_KEY "memory_pool_size"
#define WAITING_QUEUE_SIZE_KEY "waiting_queue_size"
#define MAX_WAITING_TIME_KEY "max_waiting_time"
#define LOG_DIR_KEY "log_dir"
#define DATA_DIR_KEY "data_dir"

const std::string CONFIG_FILE = PathUtils::GetTargetFilePath("conf/tinykv.conf");

class TinyKVConfig
{
private:
    std::string config_file_ = CONFIG_FILE;
    std::unordered_map<std::string, std::string> config_map_;
    TinyKVConfig()
    {
        LoadDefaultConfig();
        LoadConfig();
    }
    void LoadConfig()
    {
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
            config_map_[key] = value;
        }
    }

    void LoadDefaultConfig()
    {
        config_map_[LOG_DIR_KEY] = DEFAULT_LOG_DIR;
        config_map_[LOG_LEVEL_KEY] = std::to_string(static_cast<int>(DEFAULT_LOG_LEVEL));
        config_map_[LOG_ROTATE_SIZE_KEY] = std::to_string(DEFAULT_LOG_ROTATE_SIZE);
        config_map_[PORT_KEY] = std::to_string(DEFAULT_PORT);
        config_map_[READ_ONLY_KEY] = std::to_string(DEFAULT_READ_ONLY);
        config_map_[SKIPLIST_MAX_LEVEL_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_LEVEL);
        config_map_[SKIPLIST_PROBABILITY_KEY] = std::to_string(DEFAULT_SKIPLIST_PROBABILITY);
        config_map_[SKIPLIST_MAX_NODE_COUNT_KEY] = std::to_string(DEFAULT_SKIPLIST_MAX_NODE_COUNT);
        config_map_[MEMTABLE_SIZE_KEY] = std::to_string(DEFAULT_MEMTABLE_SIZE);
        config_map_[WAL_ENABLE_KEY] = std::to_string(DEFAULT_WAL_ENABLE);
        config_map_[WAL_DIR_KEY] = DEFAULT_WAL_DIR;
        config_map_[WAL_FILE_SIZE_KEY] = std::to_string(DEFAULT_WAL_FILE_SIZE);
        config_map_[WAL_FILE_MAX_COUNT_KEY] = std::to_string(DEFAULT_WAL_FILE_MAX_COUNT);
        config_map_[WAL_SYNC_INTERVAL_KEY] = std::to_string(DEFAULT_WAL_SYNC_INTERVAL);
        config_map_[SSTABLE_MERGE_THRESHOLD_KEY] = std::to_string(DEFAULT_SSTABLE_MERGE_THRESHOLD);
        config_map_[DATA_FLUSH_INTERVAL_KEY] = std::to_string(DEFAULT_DATA_FULSH_INTERVAL);
        config_map_[DATA_FLUSH_STRATEGY_KEY] = std::to_string(DEFAULT_DATA_FLUSH_STRATEGY);
        config_map_[MAX_CONNECTIONS_KEY] = std::to_string(DEFAULT_MAX_CONNECTIONS);
        config_map_[MEMORY_POOL_SIZE_KEY] = std::to_string(DEFAULT_MEMORY_POOL_SIZE);
        config_map_[WAITING_QUEUE_SIZE_KEY] = std::to_string(DEFAULT_WAITING_QUEUE_SIZE);
        config_map_[MAX_WAITING_TIME_KEY] = std::to_string(DEFAULT_MAX_WAITING_TIME);
        config_map_[DATA_DIR_KEY] = DEFAULT_DATA_DIR;
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
    static TinyKVConfig &GetInstance()
    {
        static TinyKVConfig instance;
        return instance;
    }
    std::optional<std::string> GetConfig(const std::string &key) const
    {
        auto it = config_map_.find(key);
        if (it != config_map_.end())
        {
            return it->second;
        }
        else
        {
            return std::nullopt;
        }
    }
    ~TinyKVConfig() = default;
};
#endif