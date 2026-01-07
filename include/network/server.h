#ifndef SERVER_H
#define SERVER_H

#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <queue>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <condition_variable>
#include <optional>
#include "common/export.h"
#include "common/threadpool.h"
#include "network/protocol/protocol.h"

// 平台差异化宏定义与头文件包含
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define SOCKET_ERROR_VALUE SOCKET_ERROR
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCKET_VALUE -1
#define SOCKET_ERROR_VALUE -1
#define CLOSE_SOCKET close
#endif

// IO复用技术头文件
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif
#undef DEFAULT_MAX_CONNECTIONS
#undef DEFAULT_CONNECT_WAIT_QUEUE_SIZE
#undef DEFAULT_CONNECT_WAIT_TIMEOUT
#undef DEFAULT_WORKER_THREAD_COUNT
#undef DEFAULT_WORKER_QUEUE_SIZE
#undef DEFAULT_WORKER_WAIT_TIMEOUT

#define DEFAULT_MAX_CONNECTIONS 1024                                        // 最大并发连接数
#define DEFAULT_CONNECT_WAIT_QUEUE_SIZE 100                                 // 等待队列最大容量
#define DEFAULT_CONNECT_WAIT_TIMEOUT 30                                     // 连接等待超时时间（秒）
#define DEFAULT_WORKER_THREAD_COUNT std::thread::hardware_concurrency() + 1 // 工作线程数量
#define DEFAULT_WORKER_QUEUE_SIZE 1000                                      // 工作线程任务队列大小
#define DEFAULT_WORKER_WAIT_TIMEOUT 30                                      // 任务提交等待超时时间（秒）

class Storage;

/**
 * @brief 连接信息结构体
 *
 * 用于存储 TCP 连接的相关信息，包括 socket 描述符、
 * 连接时间和客户端地址等。主要用于等待队列和未认证连接集合。
 */
struct Connection
{
    socket_t socket;                                  // socket描述符
    std::chrono::steady_clock::time_point start_time; // 等待开始时间/就绪开始时间
    sockaddr_in client_addr;                          // 客户端地址信息

    /**
     * @brief 相等性比较运算符
     *
     * @param other 另一个 Connection 对象
     * @return true 如果 socket 描述符相同
     */
    bool operator==(const Connection &other) const
    {
        return socket == other.socket;
    }
};

/**
 * @brief Connection 结构体的哈希函数对象
 *
 * 用于将 Connection 存储在 unordered_set 中。
 */
struct ConnectionHash
{
    /**
     * @brief 计算 Connection 的哈希值
     *
     * @param conn Connection 对象
     * @return size_t 基于 socket 描述符的哈希值
     */
    size_t operator()(const Connection &conn) const
    {
        return std::hash<socket_t>()(conn.socket);
    }
};
/**
 * @brief TCP 网络服务器类
 *
 * 实现了一个高性能的 TCP 服务器，支持：
 * - 跨平台 IO 复用（Linux epoll、macOS kqueue、Windows select）
 * - 连接池管理（最大连接数、等待队列、超时控制）
 * - 线程池处理客户端请求
 * - 可选的密码认证机制
 * - 认证超时控制
 *
 * 工作流程：
 * 1. 初始化 socket、绑定端口、监听连接
 * 2. 使用 IO 复用技术（epoll/kqueue/select）管理多个 socket
 * 3. 新连接到达时，检查连接数限制：
 *    - 未满：直接加入活跃连接池
 *    - 已满但等待队列未满：加入等待队列
 *    - 等待队列已满：拒绝连接
 * 4. 连接断开时，自动从等待队列激活一个连接
 * 5. 后台线程定期检查等待队列和未认证连接的超时
 * 6. 客户端请求提交到线程池异步处理
 */
class EYAKV_NETWORK_API EyaServer
{
public:
    /**
     * @brief 构造函数
     *
     * @param storage 存储引擎指针，用于执行键值操作
     * @param ip 监听的 IP 地址，"0.0.0.0" 表示监听所有接口
     * @param port 监听的端口号
     * @param password 服务器密码，为空表示不需要认证
     * @param max_connections 最大并发连接数（默认 1024）
     * @param connect_wait_queue_size 等待队列最大容量（默认 100）
     * @param connect_wait_timeout 连接等待超时时间，单位秒（默认 30）
     * @param worker_thread_count 工作线程数量（默认 CPU 核心数 + 1）
     * @param worker_queue_size 工作线程任务队列大小（默认 1000）
     * @param worker_wait_timeout 任务提交等待超时时间，单位秒（默认 30）
     */
    EyaServer(Storage *storage, const std::string &ip,
              const u_short port,
              const std::string &password,
              const uint32_t max_connections = DEFAULT_MAX_CONNECTIONS,
              const uint32_t connect_wait_queue_size = DEFAULT_CONNECT_WAIT_QUEUE_SIZE,
              const uint32_t connect_wait_timeout = DEFAULT_CONNECT_WAIT_TIMEOUT,
              const uint32_t worker_thread_count = DEFAULT_WORKER_THREAD_COUNT,
              const uint32_t worker_queue_size = DEFAULT_WORKER_QUEUE_SIZE,
              const uint32_t worker_wait_timeout = DEFAULT_WORKER_WAIT_TIMEOUT);

    /**
     * @brief 析构函数
     *
     * 停止所有监控线程，清理等待队列和未认证连接，
     * 关闭所有 socket 资源。
     */
    ~EyaServer();

    // 禁止拷贝和右移
    EyaServer(const EyaServer &) = delete;
    EyaServer &operator=(const EyaServer &) = delete;
    EyaServer(EyaServer &&) = delete;
    EyaServer &operator=(EyaServer &&) = delete;

    /**
     * @brief 初始化并启动服务器
     *
     * 执行以下操作：
     * 1. 创建监听 socket
     * 2. 设置 socket 为非阻塞模式
     * 3. 绑定 IP 地址和端口
     * 4. 开始监听
     * 5. 初始化 IO 复用（epoll/kqueue/select）
     * 6. 初始化工作线程池
     * 7. 启动等待队列监控线程
     * 8. 启动认证超时监控线程（如果设置了密码）
     *
     * @return true 初始化成功
     * @return false 初始化失败
     */
    bool start();

    /**
     * @brief 进入事件循环
     *
     * 这是服务器的主循环，持续监听和处理网络事件：
     * - Linux：使用 epoll_wait
     * - macOS：使用 kevent
     * - Windows：使用 select
     *
     * 循环在 is_running_ 为 true 时持续运行。
     * 新连接到达时调用 handle_accept()
     * 有数据可读时调用 handle_client()
     */
    void run();

private:
    /**
     * @brief 设置 socket 为非阻塞模式
     *
     * 非阻塞模式下的 socket 操作不会阻塞调用线程，
     * 如果没有数据可读写，会立即返回错误。
     * 这对于 IO 复用（epoll/kqueue/select）是必需的。
     *
     * @param sock 要设置的 socket 描述符
     */
    void set_non_blocking(socket_t sock);

    /**
     * @brief 处理新连接
     *
     * 当监听 socket 有新连接到达时被调用。
     * 主要逻辑：
     * 1. 调用 accept() 接受新连接
     * 2. 检查当前连接数：
     *    - 未满：直接加入活跃连接池，注册到 IO 复用
     *    - 已满：加入等待队列（如果队列未满）
     *    - 等待队列已满：拒绝连接，关闭 socket
     * 3. 如果密码认证启用，将新连接加入未认证集合
     * 4. 发送连接状态给客户端（READY/WAITING）
     *
     * 注意：Linux 边缘触发模式下需要循环 accept 直到 EAGAIN
     */
    void handle_accept();

    /**
     * @brief 处理客户端消息
     *
     * 当客户端 socket 有数据可读时被调用（在主线程中执行）。
     * 主要逻辑：
     * 1. 接收客户端数据（解析 Header 和 Body）
     * 2. 反序列化 Request 对象
     * 3. 将请求任务提交到线程池异步处理
     * 4. 如果线程池满，返回错误响应并关闭连接
     *
     * 注意：Linux 边缘触发模式需要循环读取直到 EAGAIN
     *
     * @param client_sock 客户端 socket 描述符
     */
    void handle_client(socket_t client_sock);

    /**
     * @brief 关闭 socket 并管理连接池
     *
     * 执行以下操作：
     * 1. 关闭指定 socket
     * 2. 从 IO 复用中移除（epoll_ctl/kqueue/FD_CLR）
     * 3. 减少当前连接计数
     * 4. 从未认证集合中移除（如果存在）
     * 5. 如果等待队列不为空，激活首个等待的连接：
     *    - 从等待队列弹出
     *    - 增加连接计数
     *    - 注册到 IO 复用
     *    - 加入未认证集合
     *    - 发送 READY 状态给客户端
     *
     * @param sock 要关闭的 socket 描述符
     */
    void close_socket(socket_t sock);

    /**
     * @brief 处理客户端请求（在线程池中执行）
     *
     * 该方法在工作线程中被调用，处理从客户端接收到的请求。
     * 主要功能：
     * 1. 根据请求类型进行认证或命令处理
     * 2. 调用存储引擎执行相应操作
     * 3. 构造响应并通过socket发送回客户端
     * 4. 处理各种异常情况并关闭连接
     *
     * @param request 客户端请求对象
     * @param client_sock 客户端socket描述符
     */
    void handle_request(const Request &request, socket_t client_sock);

    /**
     * @brief 发送响应给客户端
     *
     * 序列化 Response 对象并通过 socket 发送给客户端。
     * 如果发送失败或发送不完整，记录日志。
     *
     * @param response 响应对象
     * @param client_sock 客户端 socket 描述符
     */
    void send_response(const Response &response, socket_t client_sock);

    /**
     * @brief 发送连接状态给客户端
     *
     * 通知客户端当前连接的状态，用于连接池管理。
     * 状态值：
     * - READY (0): 连接已激活，可以发送请求
     * - WAITING (1): 连接在等待队列中，需要等待
     *
     * @param state 连接状态枚举值
     * @param client_sock 客户端 socket 描述符
     */
    void send_connection_state(ConnectionState state, socket_t client_sock);

private:
    // ========== 服务器配置参数 ==========
    const std::string ip_;                   // 监听的IP地址，"0.0.0.0"表示监听所有接口
    const u_short port_;                     // 监听的端口号
    const std::string password_;             // 服务器密码，为空表示不需要认证
    const uint32_t max_connections_;         // 最大并发连接数
    const uint32_t connect_wait_queue_size_; // 等待队列最大容量
    const uint32_t connect_wait_timeout_;    // 连接等待超时时间（秒）
    const uint32_t worker_thread_count_;     // 工作线程数量
    const uint32_t worker_queue_size_;       // 工作线程任务队列大小
    const uint32_t worker_wait_timeout_;     // 任务提交等待超时时间（秒）

    // ========== Socket 和运行状态 ==========
    socket_t listen_socket_;                    // 监听socket描述符
    bool is_running_;                           // 服务器运行状态标志
    std::atomic<uint32_t> current_connections_; // 当前活跃连接数统计

    // ========== 认证相关 ==========
    std::string auth_key_; // 认证密钥，客户端通过密码验证后获得

    // ========== 等待队列管理 ==========
    std::deque<Connection> wait_queue_;         // 等待队列，存储等待连接资源的客户端
    std::mutex wait_queue_mutex_;               // 等待队列互斥锁
    std::condition_variable_any wait_queue_cv_; // 条件变量，用于通知监控线程
    std::thread queue_monitor_thread_;          // 等待队列监控线程，检查连接超时
    std::atomic<bool> stop_monitor_;            // 监控线程停止标志

    // ========== 认证超时管理 ==========
    std::unordered_set<Connection, ConnectionHash> connections_without_auth_; // 未认证连接集合
    std::mutex auth_mutex_;                                                   // 认证集合互斥锁
    std::condition_variable auth_cv_;                                         // 认证条件变量，用于通知监控线程
    std::thread auth_monitor_thread_;                                         // 认证超时监控线程
    std::atomic<bool> stop_auth_monitor_;                                     // 认证监控线程停止标志

    // ========== 线程池相关 ==========
    std::unique_ptr<ThreadPool> thread_pool_; // 线程池，用于异步处理客户端请求
    Storage *storage_;                        // 存储引擎指针，用于执行键值操作

    // ========== 平台特定的IO复用句柄或数据 ==========
#ifdef __linux__
    int epoll_fd_;               // epoll 文件描述符
    struct epoll_event *events_; // epoll 事件数组，用于存储就绪事件
#elif defined(__APPLE__)
    int kqueue_fd_;             // kqueue 文件描述符
    struct kevent *event_list_; // kevent 事件列表，用于存储就绪事件
#else // Windows (使用select模型)
    fd_set master_set_; // select 主集合，存储所有需要监听的 socket
#endif
};

#endif // SERVER_H