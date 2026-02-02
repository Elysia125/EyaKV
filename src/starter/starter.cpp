#include <csignal>
#include <cstdlib>
#include <atomic>
#include "starter/starter.h"
#include "storage/storage.h"
#include "network/tcp_server.h"
#include "config/config.h"
#include "logger/logger.h"
#include "raft/raft.h"
EyaKVConfig &config = EyaKVConfig::get_instance();

EyaServer *EyaKVStarter::server = nullptr;
std::atomic<bool> EyaKVStarter::should_shutdown(false);
std::unique_ptr<std::thread> EyaKVStarter::raft_thread = nullptr;

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
    register_signal_handlers();
#ifdef _WIN32
    WSADATA wsaData;
    int wsaRes = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRes != 0)
    {
        std::cerr << "WSAStartup failed: " << wsaRes << std::endl;
        throw std::runtime_error("WSAStartup failed");
    }
#endif
    print_banner();
    initialize_logger();
    initialize_storage();
    initialize_raft();
    initialize_server();
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

void EyaKVStarter::initialize_storage()
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
    uint32_t sstable_merge_threshold = static_cast<uint32_t>(std::stoul(config.get_config(SSTABLE_MERGE_THRESHOLD_KEY).value()));
    std::optional<std::string> wal_flush_interval_str = config.get_config(WAL_FLUSH_INTERVAL_KEY);
    std::optional<uint32_t> wal_flush_interval = std::nullopt;
    if (wal_flush_interval_str.has_value())
    {
        wal_flush_interval = static_cast<uint32_t>(std::stoul(wal_flush_interval_str.value()));
    }
    std::optional<std::string> wal_flush_strategy_str = config.get_config(WAL_FLUSH_STRATEGY_KEY);
    WALFlushStrategy wal_flush_strategy = WALFlushStrategy::BACKGROUND_THREAD;
    if (wal_flush_strategy_str.has_value())
    {
        int strategy_int = std::stoi(wal_flush_strategy_str.value());
        wal_flush_strategy = static_cast<WALFlushStrategy>(strategy_int);
    }
    SSTableMergeStrategy sstable_merge_strategy = static_cast<SSTableMergeStrategy>(std::stoi(config.get_config(SSTABLE_MERGE_STRATEGY_KEY).value()));
    uint64_t sstable_zero_level_size = static_cast<uint64_t>(std::stoull(config.get_config(SSTABLE_ZERO_LEVEL_SIZE_KEY).value()));
    uint32_t sstable_level_size_ratio = static_cast<uint32_t>(std::stoul(config.get_config(SSTABLE_LEVEL_SIZE_RATIO_KEY).value()));
    Storage::init(data_dir.value(),
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
    if (Storage::get_instance() != nullptr)
    {
        std::cout << "Storage initialized. Data directory: " << data_dir.value() << std::endl;
    }
    else
    {
        std::cerr << "Failed to initialize storage." << std::endl;
        throw std::runtime_error("Failed to initialize storage.");
    }
}

void EyaKVStarter::initialize_raft()
{
    std::cout << "Initializing Raft consensus..." << std::endl;
    std::optional<std::string> ip_str = config.get_config(IP_KEY);
    if (!ip_str.has_value() || ip_str->empty())
    {
        throw std::runtime_error("IP not configured.");
    }
    std::string ip = ip_str.value();
    std::optional<std::string> port_str = config.get_config(RAFT_PORT_KEY);
    if (!port_str.has_value())
    {
        throw std::runtime_error("Port not configured.");
    }
    uint16_t port = static_cast<uint16_t>(std::stoi(port_str.value()));
    std::optional<std::string> raft_trust_ip_str = config.get_config(RAFT_TRUST_IP_KEY);
    std::unordered_set<std::string> raft_trust_ip;
    if (raft_trust_ip_str.has_value() && !raft_trust_ip_str->empty())
    {
        std::vector<std::string> ret = split(raft_trust_ip_str.value(), ',');
        for (const auto &addr : ret)
        {
            raft_trust_ip.insert(addr);
        }
    }
    std::optional<std::string> data_dir = config.get_config(DATA_DIR_KEY);
    if (!data_dir.has_value() || data_dir->empty())
    {
        throw std::runtime_error("Data directory not configured for Raft.");
    }
    RaftNode::init(data_dir.value(), ip, port, raft_trust_ip);
    if (RaftNode::get_instance() != nullptr)
    {
        std::cout << "Raft consensus initialized successfully." << std::endl;
    }
    else
    {
        std::cerr << "Failed to initialize Raft consensus." << std::endl;
        throw std::runtime_error("Failed to initialize Raft consensus.");
    }
    RaftNode::get_instance()->start();
    raft_thread = std::make_unique<std::thread>([]()
                                                { RaftNode::get_instance()->run(); });
    raft_thread->detach();
}

void EyaKVStarter::initialize_server()
{
    std::cout << "Initializing network server..." << std::endl;
    std::optional<std::string> ip_str = config.get_config(IP_KEY);
    if (!ip_str.has_value() || ip_str->empty())
    {
        throw std::runtime_error("IP not configured.");
    }
    std::optional<std::string> port_str = config.get_config(PORT_KEY);
    if (!port_str.has_value())
    {
        throw std::runtime_error("Port not configured.");
    }
    unsigned short port = static_cast<unsigned short>(std::stoi(port_str.value()));

    std::optional<std::string> password_str = config.get_config(PASSWORD_KEY);
    std::string password = password_str.has_value() ? password_str.value() : "";

    std::optional<std::string> max_connections_str = config.get_config(MAX_CONNECTIONS_KEY);
    uint32_t max_connections = max_connections_str.has_value() ? static_cast<uint32_t>(std::stoul(max_connections_str.value())) : DEFAULT_MAX_CONNECTIONS;

    std::optional<std::string> wait_queue_size_str = config.get_config(WAITING_QUEUE_SIZE_KEY);
    uint32_t wait_queue_size = wait_queue_size_str.has_value() ? static_cast<uint32_t>(std::stoul(wait_queue_size_str.value())) : DEFAULT_WAITING_QUEUE_SIZE;

    std::optional<std::string> max_waiting_time_str = config.get_config(MAX_WAITING_TIME_KEY);
    uint32_t max_waiting_time = max_waiting_time_str.has_value() ? static_cast<uint32_t>(std::stoul(max_waiting_time_str.value())) : DEFAULT_MAX_WAITING_TIME;

    std::optional<std::string> worker_thread_count_str = config.get_config(WORKER_THREAD_COUNT_KEY);
    uint32_t worker_thread_count = worker_thread_count_str.has_value() ? static_cast<uint32_t>(std::stoul(worker_thread_count_str.value())) : DEFAULT_WORKER_THREAD_COUNT;

    std::optional<std::string> worker_queue_size_str = config.get_config(WORKER_QUEUE_SIZE_KEY);
    uint32_t worker_queue_size = worker_queue_size_str.has_value() ? static_cast<uint32_t>(std::stoul(worker_queue_size_str.value())) : DEFAULT_WORKER_QUEUE_SIZE;

    std::optional<std::string> worker_wait_timeout_str = config.get_config(WORKER_WAIT_TIMEOUT_KEY);
    uint32_t worker_wait_timeout = worker_wait_timeout_str.has_value() ? static_cast<uint32_t>(std::stoul(worker_wait_timeout_str.value())) : DEFAULT_WORKER_WAIT_TIMEOUT;

    server = new EyaServer(ip_str.value(),
                           port,
                           password,
                           max_connections,
                           wait_queue_size,
                           max_waiting_time,
                           worker_thread_count,
                           worker_queue_size,
                           worker_wait_timeout);
    server->start();
    std::cout << "EyaServer initialized. Listening on " << ip_str.value() << ":" << port << std::endl;
    server->run();
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
            shutdown();
        }
        return TRUE; }, TRUE);
#else
    if (std::signal(SIGINT, [](int signum)
                    {
        LOG_INFO("Received SIGINT, shutting down...");
        shutdown(); }) == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGINT handler");
    }
    if (std::signal(SIGTERM, [](int signum)
                    {
                        LOG_INFO("Received SIGTERM, shutting down...");
                        shutdown(); }) == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGTERM handler");
    }
#endif
}
void EyaKVStarter::shutdown()
{
    if (should_shutdown.load())
    {
        return; // 避免重复关闭
    }

    should_shutdown.store(true);
    LOG_INFO("Initiating graceful shutdown...");
    // 停止raft
    if (RaftNode::get_instance() != nullptr)
    {
        LOG_INFO("Stopping Raft node...");
        RaftNode::get_instance()->stop();
        LOG_INFO("Raft node stopped");
    }
    // 停止服务器
    if (server != nullptr)
    {
        LOG_INFO("Stopping server...");
        server->stop();
        delete server;
        server = nullptr;
        LOG_INFO("Server stopped");
    }

    LOG_INFO("Graceful shutdown completed");
    exit(EXIT_SUCCESS);
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
        shutdown();
    }
}