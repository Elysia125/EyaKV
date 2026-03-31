#include "starter/starter.h"
#include "raft/raft.h"
#include "network/tcp_server.h"
#include "storage/storage.h"
#include "config/config.h"
#include "logger/logger.h"

#include <csignal>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// 外部单例引用
extern EyaKVConfig &config; // 假设 config 已经在其他地方实例化，最好通过 EyaKVConfig::get_instance() 获取

namespace
{
    // 获取全局配置单例
    EyaKVConfig &GetConfig()
    {
        return EyaKVConfig::get_instance();
    }

    /**
     * @brief 辅助函数：获取必须存在的配置项，缺失则抛出异常
     */
    std::string GetRequiredConfig(const std::string &key)
    {
        auto opt_val = GetConfig().get_config(key);
        if (!opt_val.has_value() || opt_val->empty())
        {
            throw std::runtime_error("Required configuration missing: " + key);
        }
        return opt_val.value();
    }

    /**
     * @brief 辅助函数：获取可选的配置项并进行类型转换
     */
    template <typename T>
    std::optional<T> GetOptConfig(const std::string &key)
    {
        auto opt_val = GetConfig().get_config(key);
        if (!opt_val.has_value() || opt_val->empty())
        {
            return std::nullopt;
        }
        try
        {
            if constexpr (std::is_same_v<T, std::string>)
                return opt_val.value();
            else if constexpr (std::is_same_v<T, bool>)
                return (opt_val.value() == "1" || opt_val.value() == "true");
            else if constexpr (std::is_same_v<T, int>)
                return std::stoi(opt_val.value());
            else if constexpr (std::is_same_v<T, uint32_t>)
                return static_cast<uint32_t>(std::stoul(opt_val.value()));
            else if constexpr (std::is_same_v<T, uint64_t>)
                return static_cast<uint64_t>(std::stoull(opt_val.value()));
            else if constexpr (std::is_same_v<T, double>)
                return std::stod(opt_val.value());
        }
        catch (const std::exception &e)
        {
            std::cerr << "Config parsing error for key [" << key << "]: " << e.what() << std::endl;
        }
        return std::nullopt;
    }

    /**
     * @brief 辅助函数：获取带有默认值的配置项
     */
    template <typename T>
    T GetConfigOrDefault(const std::string &key, const T &default_val)
    {
        auto opt_val = GetOptConfig<T>(key);
        return opt_val.value_or(default_val);
    }

    /**
     * @brief 辅助函数：如果配置项存在，则应用传入的 Lambda 进行赋值
     */
    template <typename T, typename Func>
    void ApplyConfigIfExists(const std::string &key, Func apply_func)
    {
        if (auto val = GetOptConfig<T>(key))
        {
            apply_func(val.value());
        }
    }
}

// 静态成员变量初始化
std::unique_ptr<EyaServer> EyaKVStarter::server_ = nullptr;
std::unique_ptr<std::thread> EyaKVStarter::raft_thread_ = nullptr;
std::atomic<bool> EyaKVStarter::should_shutdown_{false};
std::atomic<bool> EyaKVStarter::has_shutdown_{false};

void EyaKVStarter::print_banner()
{
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole != INVALID_HANDLE_VALUE)
    {
        DWORD mode = 0;
        if (GetConsoleMode(hConsole, &mode))
        {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hConsole, mode);
        }
    }
#endif

    const std::string PINK = "\033[1;38;5;213m";
    const std::string RESET = "\033[0m";

    const std::string asciiArt = R"(
        ███████╗██╗   ██╗ █████╗     ██╗  ██╗██╗   ██╗
        ██╔════╝╚██╗ ██╔╝██╔══██╗    ██║ ██╔╝██║   ██║
        █████╗   ╚████╔╝ ███████║    █████╔╝ ██║   ██║
        ██╔══╝    ╚██╔╝  ██╔══██║    ██╔═██╗ ╚██╗ ██╔╝
        ███████╗   ██║   ██║  ██║    ██║  ██╗ ╚████╔╝ 
        ╚══════╝   ╚═╝   ╚═╝  ╚═╝    ╚═╝  ╚═╝  ╚═══╝  
    )";
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
        throw std::runtime_error("WSAStartup failed with error code: " + std::to_string(wsaRes));
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
    LoggerConfig logger_config;
    logger_config.log_dir = GetRequiredConfig(LOG_DIR_KEY);
    int level_int = GetConfigOrDefault<int>(LOG_LEVEL_KEY, static_cast<int>(LogLevel::INFO));

    LogLevel log_level = LogLevel::INFO;
    if (level_int >= static_cast<int>(LogLevel::DEBUG) && level_int <= static_cast<int>(LogLevel::FATAL))
    {
        log_level = static_cast<LogLevel>(level_int);
    }
    logger_config.level = log_level;
    auto rotate_size = GetOptConfig<uint32_t>(LOG_ROTATE_SIZE_KEY);

    if (rotate_size.has_value())
    {
        logger_config.rotate_size_mb = rotate_size.value();
    }
    bool enable_console = GetConfigOrDefault<bool>(LOG_CONSOLE_ENABLED_KEY, true);
    logger_config.enable_console = enable_console;
    Logger::SetConfig(logger_config);
    Logger::Init();
    // std::cout << "Logger initialized. Directory: " << logger_config.log_dir << ", Level: " << static_cast<int>(logger_config.level) << ", Console: " << logger_config.enable_console << std::endl;
}

void EyaKVStarter::initialize_storage()
{
    std::cout << "Initializing storage..." << std::endl;

    std::string data_dir = GetRequiredConfig(DATA_DIR_KEY);
    std::string wal_dir = GetRequiredConfig(WAL_DIR_KEY);

    bool read_only = GetConfigOrDefault<bool>(READ_ONLY_KEY, false);
    bool wal_enable = GetConfigOrDefault<bool>(WAL_ENABLE_KEY, true);

    size_t memtable_size = GetConfigOrDefault<uint32_t>(MEMTABLE_SIZE_KEY, 0);
    size_t skiplist_max_level = GetConfigOrDefault<uint32_t>(SKIPLIST_MAX_LEVEL_KEY, 0);
    double skiplist_prob = GetConfigOrDefault<double>(SKIPLIST_PROBABILITY_KEY, 0.5);
    uint32_t sstable_merge_thresh = GetConfigOrDefault<uint32_t>(SSTABLE_MERGE_THRESHOLD_KEY, 0);

    auto wal_flush_interval = GetOptConfig<uint32_t>(WAL_FLUSH_INTERVAL_KEY);

    WALFlushStrategy wal_flush_strategy = static_cast<WALFlushStrategy>(
        GetConfigOrDefault<int>(WAL_FLUSH_STRATEGY_KEY, static_cast<int>(WALFlushStrategy::BACKGROUND_THREAD)));

    SSTableMergeStrategy sstable_merge_strategy = static_cast<SSTableMergeStrategy>(
        GetConfigOrDefault<int>(SSTABLE_MERGE_STRATEGY_KEY, 0));

    uint64_t sstable_zero_level_size = GetConfigOrDefault<uint64_t>(SSTABLE_ZERO_LEVEL_SIZE_KEY, 0);
    uint32_t sstable_level_size_ratio = GetConfigOrDefault<uint32_t>(SSTABLE_LEVEL_SIZE_RATIO_KEY, 10);

    Storage::init(data_dir, wal_dir, read_only, wal_enable, wal_flush_interval,
                  wal_flush_strategy, memtable_size, skiplist_max_level,
                  skiplist_prob, sstable_merge_strategy, sstable_merge_thresh,
                  sstable_zero_level_size, sstable_level_size_ratio);

    if (Storage::get_instance() == nullptr)
    {
        throw std::runtime_error("Failed to initialize storage.");
    }
    std::cout << "Storage initialized. Data directory: " << data_dir << std::endl;
}

void EyaKVStarter::initialize_raft()
{
    std::cout << "Initializing Raft consensus..." << std::endl;

    std::string ip = GetRequiredConfig(IP_KEY);
    uint16_t port = static_cast<uint16_t>(GetRequiredConfig(RAFT_PORT_KEY).empty() ? 0 : std::stoi(GetRequiredConfig(RAFT_PORT_KEY)));
    std::string data_dir = GetRequiredConfig(DATA_DIR_KEY);

    std::unordered_set<std::string> raft_trust_ip;
    if (auto trust_ips_str = GetOptConfig<std::string>(RAFT_TRUST_IP_KEY))
    {
        std::vector<std::string> ips = split(trust_ips_str.value(), ','); // 假设 split 是全局辅助函数
        raft_trust_ip.insert(ips.begin(), ips.end());
    }

    RaftNodeConfig raft_cfg;

    // 使用 Lambda 辅助方法，仅当配置存在时覆写 raft_cfg 里的默认值，极大提升可读性
    ApplyConfigIfExists<int>(RAFT_ELECTION_TIMEOUT_MIN_KEY, [&](int v)
                             { raft_cfg.election_timeout_min_ms = v; });
    ApplyConfigIfExists<int>(RAFT_ELECTION_TIMEOUT_MAX_KEY, [&](int v)
                             { raft_cfg.election_timeout_max_ms = v; });
    ApplyConfigIfExists<int>(RAFT_HEARTBEAT_INTERVAL_KEY, [&](int v)
                             { raft_cfg.heartbeat_interval_ms = v; });
    ApplyConfigIfExists<int>(RAFT_RPC_TIMEOUT_KEY, [&](int v)
                             { raft_cfg.raft_rpc_timeout_ms = v; });
    ApplyConfigIfExists<int>(RAFT_FOLLOWER_IDLE_WAIT_KEY, [&](int v)
                             { raft_cfg.follower_idle_wait_ms = v; });
    ApplyConfigIfExists<int>(RAFT_JOIN_MAX_RETRIES_KEY, [&](int v)
                             { raft_cfg.join_cluster_max_retries = v; });
    ApplyConfigIfExists<int>(RAFT_REQUEST_VOTE_TIMEOUT_KEY, [&](int v)
                             { raft_cfg.request_vote_recv_timeout_ms = v; });
    ApplyConfigIfExists<int>(RAFT_SUBMIT_TIMEOUT_KEY, [&](int v)
                             { raft_cfg.submit_command_timeout_ms = v; });
    ApplyConfigIfExists<uint32_t>(RAFT_APPEND_BATCH_KEY, [&](uint32_t v)
                                  { raft_cfg.append_entries_max_batch = v; });
    ApplyConfigIfExists<uint64_t>(RAFT_SNAPSHOT_CHUNK_KEY, [&](uint64_t v)
                                  { raft_cfg.snapshot_chunk_size_bytes = v; });
    ApplyConfigIfExists<bool>(RAFT_NEED_MAJORITY_CONFIRM_KEY, [&](bool v)
                              { raft_cfg.need_majority_confirm = v; });
    ApplyConfigIfExists<uint64_t>(RAFT_RESULT_CACHE_CAPACITY_KEY, [&](uint64_t v)
                                  { raft_cfg.result_cache_capacity = v; });
    ApplyConfigIfExists<uint32_t>(BATCH_TIMEOUT_KEY, [&](uint32_t v)
                                  { raft_cfg.batch_command_timeout_ms = v; });

    // 线程池配置
    ApplyConfigIfExists<uint32_t>(RAFT_THREADPOOL_WORKERS_KEY, [&](uint32_t v)
                                  { raft_cfg.thread_pool_config.thread_count = v; });
    ApplyConfigIfExists<uint32_t>(RAFT_THREADPOOL_QUEUE_KEY, [&](uint32_t v)
                                  { raft_cfg.thread_pool_config.queue_size = v; });
    ApplyConfigIfExists<uint32_t>(RAFT_THREADPOOL_WAIT_KEY, [&](uint32_t v)
                                  { raft_cfg.thread_pool_config.wait_timeout_ms = v; });

    // 日志存储配置
    ApplyConfigIfExists<uint32_t>(RAFT_LOG_THRESHOLD_KEY, [&](uint32_t v)
                                  { raft_cfg.log_config.log_size_threshold = v; });
    ApplyConfigIfExists<double>(RAFT_LOG_TRUNCATE_RATIO_KEY, [&](double v)
                                { raft_cfg.log_config.truncate_ratio = v; });
    ApplyConfigIfExists<std::string>(RAFT_WAL_FILENAME_KEY, [&](const std::string &v)
                                     { raft_cfg.log_config.wal_filename = v; });
    ApplyConfigIfExists<std::string>(RAFT_INDEX_FILENAME_KEY, [&](const std::string &v)
                                     { raft_cfg.log_config.index_filename = v; });

    RaftNode::init(data_dir, ip, port, raft_trust_ip, 5, raft_cfg);
    if (RaftNode::get_instance() == nullptr)
    {
        throw std::runtime_error("Failed to initialize Raft consensus.");
    }

    std::cout << "Raft consensus initialized successfully." << std::endl;
    RaftNode::get_instance()->start();

    raft_thread_ = std::make_unique<std::thread>([]()
                                                 { RaftNode::get_instance()->run(); });
    // 使用 detach 使其在后台运行，但在 shutdown 阶段我们依靠单例里的 stop 触发退出
    raft_thread_->detach();
}

void EyaKVStarter::initialize_server()
{
    std::cout << "Initializing network server..." << std::endl;

    std::string ip = GetRequiredConfig(IP_KEY);
    uint16_t port = static_cast<uint16_t>(GetConfigOrDefault<int>(PORT_KEY, 0));
    std::string password = GetConfigOrDefault<std::string>(PASSWORD_KEY, "");

    uint32_t max_conn = GetConfigOrDefault<uint32_t>(MAX_CONNECTIONS_KEY, DEFAULT_MAX_CONNECTIONS);
    uint32_t wait_queue_size = GetConfigOrDefault<uint32_t>(WAITING_QUEUE_SIZE_KEY, DEFAULT_WAITING_QUEUE_SIZE);
    uint32_t max_waiting_time = GetConfigOrDefault<uint32_t>(MAX_WAITING_TIME_KEY, DEFAULT_MAX_WAITING_TIME);
    uint32_t worker_threads = GetConfigOrDefault<uint32_t>(WORKER_THREAD_COUNT_KEY, DEFAULT_WORKER_THREAD_COUNT);
    uint32_t worker_queue = GetConfigOrDefault<uint32_t>(WORKER_QUEUE_SIZE_KEY, DEFAULT_WORKER_QUEUE_SIZE);
    uint32_t worker_timeout = GetConfigOrDefault<uint32_t>(WORKER_WAIT_TIMEOUT_KEY, DEFAULT_WORKER_WAIT_TIMEOUT);

    // 利用 std::make_unique 安全分配堆内存
    server_ = std::make_unique<EyaServer>(
        ip, port, password, max_conn, wait_queue_size,
        max_waiting_time, worker_threads, worker_queue, worker_timeout);

    server_->start();
    std::cout << "EyaServer initialized. Listening on " << ip << ":" << port << std::endl;

    // run 预期是阻塞的
    server_->run();
}

void EyaKVStarter::register_signal_handlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL
                          {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
            should_shutdown_.store(true, std::memory_order_relaxed);
            return TRUE;
        }
        return FALSE; }, TRUE);
#else
    auto signal_handler = [](int /*signum*/)
    {
        should_shutdown_.store(true, std::memory_order_relaxed);
    };
    if (std::signal(SIGINT, signal_handler) == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGINT handler");
    }
    if (std::signal(SIGTERM, signal_handler) == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGTERM handler");
    }
#endif

    std::thread watcher_thread(&EyaKVStarter::background_watcher_thread);
    watcher_thread.detach();
}

void EyaKVStarter::background_watcher_thread()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (should_shutdown_.load(std::memory_order_relaxed))
        {
            EyaKVStarter::shutdown();
            break;
        }
    }
}

void EyaKVStarter::shutdown()
{
    // 利用 CAS (Compare-And-Swap) 或 exchange 确保 shutdown 的逻辑严格单次执行
    bool expected = false;
    if (!has_shutdown_.compare_exchange_strong(expected, true, std::memory_order_relaxed))
    {
        return;
    }

    LOG_INFO("Initiating graceful shutdown...");
    // Logger::Flush(); // 确保所有日志都被写入磁盘
    if (server_)
    {
        LOG_INFO("Stopping server...");
        server_->stop();
        // server_.reset(); // 安全析构
        LOG_INFO("Server stopped");
    }

    if (RaftNode::get_instance() != nullptr)
    {
        LOG_INFO("Stopping Raft node...");
        RaftNode::get_instance()->stop();
        LOG_INFO("Raft node stopped");
    }

    LOG_INFO("Graceful shutdown completed");
    Logger::Shutdown(); // 关闭日志系统，释放资源
    // exit(EXIT_SUCCESS);
}

void EyaKVStarter::start()
{
    try
    {
        initialize();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Fatal error during startup: {}", e.what());
        // 捕获异常后主动关闭清理环境
        shutdown();
    }
}