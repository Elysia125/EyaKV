#include <csignal>
#include <cstdlib>
#include "starter/starter.h"
#include "storage/storage.h"
#include "network/server.h"
#include "config/config.h"
#include "logger/logger.h"

EyaKVConfig &config = EyaKVConfig::get_instance();
Storage *EyaKVStarter::storage = nullptr;
void EyaKVStarter::print_banner()
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

void EyaKVStarter::initialize()
{
    print_banner();
    initialize_logger();
    storage = initialize_storage();
    initialize_server();
    register_signal_handlers();
}

void EyaKVStarter::initialize_logger()
{
    std::optional<std::string> log_dir = config.get_config(LOG_DIR_KEY);
    std::optional<std::string> log_level_str = config.get_config(LOG_LEVEL_KEY);
    std::optional<std::string> log_rotate_size_str = config.get_config(LOG_ROTATE_SIZE_KEY);
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

Storage *EyaKVStarter::initialize_storage()
{
    std::cout << "Initializing storage..." << std::endl;
    std::optional<std::string> data_dir = config.get_config(DATA_DIR_KEY);
    if (!data_dir.has_value() || data_dir->empty())
    {
        throw std::runtime_error("Data directory not configured.");
    }
    std::optional<std::string> wal_dir = config.get_config(WAL_DIR_KEY);
    if (!wal_dir.has_value() || wal_dir->empty())
    {
        throw std::runtime_error("WAL directory not configured.");
    }
    std::optional<std::string> read_only_str = config.get_config(READ_ONLY_KEY);
    bool read_only = false;
    if (read_only_str.has_value())
    {
        read_only = (read_only_str.value() == "1" || read_only_str.value() == "true");
    }
    std::optional<std::string> wal_enable_str = config.get_config(WAL_ENABLE_KEY);
    bool wal_enable = true;
    if (wal_enable_str.has_value())
    {
        wal_enable = (wal_enable_str.value() == "1" || wal_enable_str.value() == "true");
    }
    size_t memtable_size = static_cast<size_t>(std::stoul(config.get_config(MEMTABLE_SIZE_KEY).value()));
    size_t skiplist_max_level = static_cast<size_t>(std::stoul(config.get_config(SKIPLIST_MAX_LEVEL_KEY).value()));
    double skiplist_probability = std::stod(config.get_config(SKIPLIST_PROBABILITY_KEY).value());
    unsigned int sstable_merge_threshold = static_cast<unsigned int>(std::stoul(config.get_config(SSTABLE_MERGE_THRESHOLD_KEY).value()));
    std::optional<std::string> wal_flush_interval_str = config.get_config(WAL_FLUSH_INTERVAL_KEY);
    std::optional<unsigned int> wal_flush_interval = std::nullopt;
    if (wal_flush_interval_str.has_value())
    {
        wal_flush_interval = static_cast<unsigned int>(std::stoul(wal_flush_interval_str.value()));
    }
    std::optional<std::string> wal_flush_strategy_str = config.get_config(WAL_FLUSH_STRATEGY_KEY);
    WALFlushStrategy wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD;
    if (wal_flush_strategy_str.has_value())
    {
        int strategy_int = std::stoi(wal_flush_strategy_str.value());
        wal_flush_strategy = static_cast<WALFlushStrategy>(strategy_int);
    }
    SSTableMergeStrategy sstable_merge_strategy = static_cast<SSTableMergeStrategy>(std::stoi(config.get_config(SSTABLE_MERGE_STRATEGY_KEY).value()));
    uint32_t sstable_zero_level_size = static_cast<uint32_t>(std::stoul(config.get_config(SSTABLE_ZERO_LEVEL_SIZE_KEY).value()));
    uint32_t sstable_level_size_ratio = static_cast<uint32_t>(std::stoul(config.get_config(SSTABLE_LEVEL_SIZE_RATIO_KEY).value()));
    static Storage *st = new Storage(data_dir.value(),
                                     wal_dir.value(),
                                     read_only,
                                     wal_enable,
                                     wal_flush_interval,
                                     wal_flush_strategy,
                                     memtable_size,
                                     skiplist_max_level,
                                     skiplist_probability,
                                     sstable_merge_strategy,
                                     sstable_merge_threshold,
                                     sstable_zero_level_size,
                                     sstable_level_size_ratio);
    std::cout << "Storage initialized. Data directory: " << data_dir.value() << std::endl;
    return st;
}

void EyaKVStarter::initialize_server()
{
    std::cout << "Initializing network server..." << std::endl;
    std::optional<std::string> port_str = config.get_config(PORT_KEY);
    if (!port_str.has_value())
    {
        throw std::runtime_error("Port not configured.");
    }
    unsigned short port = static_cast<unsigned short>(std::stoi(port_str.value()));
    static EyaServer server(storage, port);
    std::cout << "EyaServer initialized. Listening on port: " << port << std::endl;
    server.Run();
}

void EyaKVStarter::register_signal_handlers()
{
    // 注册信号处理函数，例如 SIGINT 和 SIGTERM
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL
                          {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT)
        {
            LOG_INFO("Received termination signal, shutting down...");
            exit(EXIT_SUCCESS);
        }
        return FALSE; }, TRUE);
#else
    if (std::signal(SIGINT, [](int signum)
                    {
        LOG_INFO("Received SIGINT, shutting down...");
        exit(EXIT_SUCCESS); }) == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGINT handler");
    }
    if (std::signal(SIGTERM, [](int signum)
                    {
        LOG_INFO("Received SIGTERM, shutting down...");
        exit(EXIT_SUCCESS); }))== SIG_ERR)
        {
            LOG_ERROR("Failed to register SIGTERM handler");
        }
#endif
}
void EyaKVStarter::start()
{
    try
    {
        initialize();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Fatal error: %s", e.what());
        exit(EXIT_FAILURE);
    }
}