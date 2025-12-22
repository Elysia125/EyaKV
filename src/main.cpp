#include <iostream>
#include <thread>
#include "storage/storage.h"
#include "network/server.h"
#include "config/config.h"
#include "logger/logger.h"

EyaKVConfig &config = EyaKVConfig::GetInstance();
void print_banner()
{
    // 定义颜色（粉色/洋红色）
    std::string PINK = "\033[1;38;5;213m";
    std::string RESET = "\033[0m";

    // 使用 R"(...)" 原始字符串字面量，可以直接保留艺术字的格式
    std::string asciiArt = R"(
        ███████╗██╗   ██╗ █████╗     ██╗  ██╗██╗   ██╗
        ██╔════╝╚██╗ ██╔╝██╔══██╗    ██║ ██╔╝██║   ██║
        █████╗   ╚████╔╝ ███████║    █████╔╝ ██║   ██║
        ██╔══╝    ╚██╔╝  ██╔══██║    ██╔═██╗ ╚██╗ ██╔╝
        ███████╗   ██║   ██║  ██║    ██║  ██╗ ╚████╔╝ 
        ╚══════╝   ╚═╝   ╚═╝  ╚═╝    ╚═╝  ╚═╝  ╚═══╝  
    )";
    // 打印带有颜色的文字
    std::cout << PINK << asciiArt << RESET << std::endl;
}
void init_logger()
{
    std::optional<std::string> log_dir = config.GetConfig(LOG_DIR_KEY);
    std::optional<std::string> log_level_str = config.GetConfig(LOG_LEVEL_KEY);
    std::optional<std::string> log_rotate_size_str = config.GetConfig(LOG_ROTATE_SIZE_KEY);
    LogLevel log_level = LogLevel::INFO;
    if (log_level_str.has_value())
    {
        int level_int = std::stoi(log_level_str.value());
        if (level_int >= static_cast<int>(LogLevel::DEBUG) && level_int <= static_cast<int>(LogLevel::FATAL))
        {
            log_level = static_cast<LogLevel>(level_int);
        }
    }

    if (log_dir.has_value())
    {
        if (log_rotate_size_str.has_value())
        {
            unsigned long rotate_size = std::stoul(log_rotate_size_str.value());
            Logger::GetInstance().Init(log_dir.value(), log_level, rotate_size);
        }
        else
        {
            Logger::GetInstance().Init(log_dir.value(), log_level);
        }
        std::cout << "Logger initialized. Log directory: " << log_dir.value() << ", Log level: " << static_cast<int>(log_level) << std::endl;
    }
    else
    {
        throw std::runtime_error("Log directory not configured.");
    }
}
Storage &init_storage()
{
    std::cout << "Initializing storage..." << std::endl;
    std::optional<std::string> data_dir = config.GetConfig(DATA_DIR_KEY);
    if (!data_dir.has_value() || data_dir->empty())
    {
        throw std::runtime_error("Data directory not configured.");
    }
    std::optional<std::string> wal_dir = config.GetConfig(WAL_DIR_KEY);
    if (!wal_dir.has_value() || wal_dir->empty())
    {
        throw std::runtime_error("WAL directory not configured.");
    }
    std::optional<std::string> read_only_str = config.GetConfig(READ_ONLY_KEY);
    bool read_only = false;
    if (read_only_str.has_value())
    {
        read_only = (read_only_str.value() == "1" || read_only_str.value() == "true");
    }
    std::optional<std::string> wal_enable_str = config.GetConfig(WAL_ENABLE_KEY);
    bool wal_enable = true;
    if (wal_enable_str.has_value())
    {
        wal_enable = (wal_enable_str.value() == "1" || wal_enable_str.value() == "true");
    }
    u_long wal_file_size = strtoul(config.GetConfig(WAL_FILE_SIZE_KEY).value().c_str(), nullptr, 10);
    u_long max_wal_file_count = strtoul(config.GetConfig(WAL_FILE_MAX_COUNT_KEY).value().c_str(), nullptr, 10);
    u_int wal_sync_interval = static_cast<u_int>(std::stoul(config.GetConfig(WAL_SYNC_INTERVAL_KEY).value()));
    size_t memtable_size = static_cast<size_t>(std::stoul(config.GetConfig(MEMTABLE_SIZE_KEY).value()));
    size_t skiplist_max_level = static_cast<size_t>(std::stoul(config.GetConfig(SKIPLIST_MAX_LEVEL_KEY).value()));
    double skiplist_probability = std::stod(config.GetConfig(SKIPLIST_PROBABILITY_KEY).value());
    size_t skiplist_max_node_count = static_cast<size_t>(std::stoul(config.GetConfig(SKIPLIST_MAX_NODE_COUNT_KEY).value()));
    unsigned int sstable_merge_threshold = static_cast<unsigned int>(std::stoul(config.GetConfig(SSTABLE_MERGE_THRESHOLD_KEY).value()));
    std::optional<std::string> data_flush_interval_str = config.GetConfig(DATA_FLUSH_INTERVAL_KEY);
    std::optional<unsigned int> data_flush_interval = std::nullopt;
    if (data_flush_interval_str.has_value())
    {
        data_flush_interval = static_cast<unsigned int>(std::stoul(data_flush_interval_str.value()));
    }
    std::optional<std::string> data_flush_strategy_str = config.GetConfig(DATA_FLUSH_STRATEGY_KEY);
    DataFlushStrategy data_flush_strategy = DataFlushStrategy::BACKGROUND_THREAD;
    if (data_flush_strategy_str.has_value())
    {
        int strategy_int = std::stoi(data_flush_strategy_str.value());
        data_flush_strategy = static_cast<DataFlushStrategy>(strategy_int);
    }
    static Storage storage(data_dir.value(),
                           wal_dir.value(),
                           read_only,
                           wal_enable,
                           wal_file_size,
                           max_wal_file_count,
                           wal_sync_interval,
                           memtable_size,
                           skiplist_max_level,
                           skiplist_probability,
                           skiplist_max_node_count,
                           sstable_merge_threshold,
                           data_flush_interval,
                           data_flush_strategy);
    std::cout << "Storage initialized. Data directory: " << data_dir.value() << std::endl;
    return storage;
}

void init_server(Storage &storage)
{
    std::cout << "Initializing network server..." << std::endl;
    std::optional<std::string> port_str = config.GetConfig(PORT_KEY);
    if (!port_str.has_value())
    {
        throw std::runtime_error("Port not configured.");
    }
    unsigned short port = static_cast<unsigned short>(std::stoi(port_str.value()));
    static Server server(&storage, port);
    std::cout << "Server initialized. Listening on port: " << port << std::endl;
    server.Run();
}

int main(int argc, char **argv)
{
    std::cout << "EyaKV 0.1.0 starting..." << std::endl;
    print_banner();
    try
    {
        init_logger();
        Storage &storage = init_storage();
        init_server(storage);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}