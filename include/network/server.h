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
#include "common/export.h"
#include "common/threadpool.h"
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
#define DEFAULT_MAX_CONNECTIONS 1024
#define DEFAULT_CONNECT_WAIT_QUEUE_SIZE 100
#define DEFAULT_CONNECT_WAIT_TIMEOUT 30
#define DEFAULT_WORKER_THREAD_COUNT std::thread::hardware_concurrency() + 1
#define DEFAULT_WORKER_QUEUE_SIZE 1000
#define DEFAULT_WORKER_WAIT_TIMEOUT 30

class Storage;

class EYAKV_NETWORK_API EyaServer
{
public:
    EyaServer(Storage *storage, const std::string &ip,
              const u_short port,
              const std::string &password,
              const uint32_t max_connections = DEFAULT_MAX_CONNECTIONS,
              const uint32_t connect_wait_queue_size = DEFAULT_CONNECT_WAIT_QUEUE_SIZE,
              const uint32_t connect_wait_timeout = DEFAULT_CONNECT_WAIT_TIMEOUT,
              const uint32_t worker_thread_count = DEFAULT_WORKER_THREAD_COUNT,
              const uint32_t worker_queue_size = DEFAULT_WORKER_QUEUE_SIZE,
              const uint32_t worker_wait_timeout = DEFAULT_WORKER_WAIT_TIMEOUT);
    ~EyaServer();
    // 禁止拷贝和右移
    EyaServer(const EyaServer &) = delete;
    EyaServer &operator=(const EyaServer &) = delete;
    EyaServer(EyaServer &&) = delete;
    EyaServer &operator=(EyaServer &&) = delete;
    // 初始化并启动服务器
    bool start();
    // 进入事件循环
    void run();

private:
    // 设置Socket为非阻塞模式
    void set_non_blocking(socket_t sock);
    // 处理新连接
    void handle_accept();
    // 处理客户端消息
    void handle_client(socket_t client_sock);
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
    void send_response(const Response &response, socket_t client_sock);

private:
    const std::string ip_;
    const u_short port_;
    const std::string password_;
    const uint32_t max_connections_;
    const uint32_t connect_wait_queue_size_;
    const uint32_t connect_wait_timeout_; // 连接等待超时时间（秒）
    const uint32_t worker_thread_count_;
    const uint32_t worker_queue_size_;
    const uint32_t worker_wait_timeout_;
    std::string auth_key_;
    std::queue<socket_t> wait_queue_;
    std::shared_mutex wait_queue_mutex_;
    socket_t listen_socket_;
    bool is_running_;
    std::atomic<uint32_t> current_connections_; // 当前连接数统计

    // 线程池相关成员
    std::unique_ptr<ThreadPool> thread_pool_; // 线程池指针
    Storage *storage_;                        // 存储引擎指针

    // 平台特定的IO复用句柄或数据
#ifdef __linux__
    int epoll_fd_;
    struct epoll_event *events_;
#elif defined(__APPLE__)
    int kqueue_fd_;
    struct kevent *event_list_; // 用于接收事件
#else // Windows (使用select模型)
    fd_set master_set_;
#endif
};

#endif // SERVER_H