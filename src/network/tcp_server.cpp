#include "network/tcp_server.h"
#include "raft/raft.h"
#include "logger/logger.h"
#include "common/util/string_utils.h"
#include "common/types/operation_type.h"
#include <iostream>
#include <cstring>

#define HEADER_SIZE_LIMIT 1024 * 1024
#ifdef __linux__
#define INITIAL_BUFFER_SIZE 8096
#endif
EyaServer::EyaServer(const std::string &ip,
                     const u_short port,
                     const std::string &password,
                     const uint32_t max_connections,
                     const uint32_t connect_wait_queue_size,
                     const uint32_t connect_wait_timeout,
                     const uint32_t worker_thread_count,
                     const uint32_t worker_queue_size,
                     const uint32_t worker_wait_timeout)
    : TCPServer(ip, port, max_connections, connect_wait_queue_size, connect_wait_timeout),
      password_(password),
      worker_thread_count_(worker_thread_count),
      worker_queue_size_(worker_queue_size),
      worker_wait_timeout_(worker_wait_timeout),
      stop_auth_monitor_(false)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    FD_ZERO(&master_set_);
#elif defined(__linux__)
    events_ = new epoll_event[max_connections_];
#elif defined(__APPLE__)
    event_list_ = new kevent[max_connections_];
#endif
    if (!password_.empty())
    {
        auth_key_ = generate_random_string(32);
    }
}

EyaServer::~EyaServer()
{
    stop();
}

void EyaServer::stop()
{
    stop_auth_monitor_ = true;
    auth_cv_.notify_all();

    if (auth_monitor_thread_.joinable())
    {
        auth_monitor_thread_.join();
    }
    TCPServer::stop();
}

void EyaServer::start()
{
    TCPServer::start();
    is_running_ = false;
    // 初始化线程池
    ThreadPool::Config pool_config{
        worker_thread_count_,       // 工作线程数量
        worker_queue_size_,         // 任务队列大小
        worker_wait_timeout_ * 1000 // 等待超时时间（毫秒）
    };
    try
    {
        thread_pool_ = std::make_unique<ThreadPool>(pool_config);
        LOG_INFO("ThreadPool initialized with %d threads", worker_thread_count_);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to initialize ThreadPool: %s", e.what());
        throw std::runtime_error("Failed to initialize ThreadPool:" + std::string(e.what()));
    }
    // 启动认证线程
    if (!password_.empty())
    {
        auth_monitor_thread_ = std::thread([this]()
                                           {
            while (!stop_auth_monitor_.load(std::memory_order_relaxed))
            {
                std::unique_lock<std::mutex> lock(auth_mutex_);

                // 只等待停止信号或超时
                auth_cv_.wait_for(lock, std::chrono::seconds(2), [this]()
                              { return stop_auth_monitor_.load(); });
                if (stop_auth_monitor_.load(std::memory_order_relaxed))
                {
                    break;
                }
                
                std::vector<socket_t> sockets_to_close;
                auto now = std::chrono::steady_clock::now();
                for (auto it = connections_without_auth_.begin(); it != connections_without_auth_.end();)
                {
                    if (now - it->start_time > std::chrono::seconds(2))
                    {
                        LOG_WARN("Connection without auth timeout, closing socket");
                        sockets_to_close.push_back(it->socket);
                        it = connections_without_auth_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                lock.unlock(); // Explicitly unlock before calling close_socket

                for (auto sock : sockets_to_close)
                {
                    close_socket(sock);
                }
            } });
    }
    is_running_ = true;
}

void EyaServer::handle_accept()
{
#ifdef __linux__
    // 边缘触发模式下需要循环 accept 直到返回 EAGAIN
    while (true)
    {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        socket_t client_sock = accept(listen_socket_, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET_VALUE)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; // 所有连接已处理完毕
            }
            LOG_ERROR("Accept error: %s", strerror(errno));
            return;
        }

        // 检查连接数限制和等待队列
        std::unique_lock<std::mutex> lock(wait_queue_mutex_);

        if (current_connections_ < max_connections_)
        {
            // 连接数未满，直接接受
            set_non_blocking(client_sock);

            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
            LOG_INFO("New connection accepted: %s:%d", clientIp, ntohs(client_addr.sin_port));

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
            ev.data.fd = client_sock;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_sock, &ev) == -1)
            {
                LOG_ERROR("Epoll ctl failed for client socket: %s", strerror(errno));
                CLOSE_SOCKET(client_sock);
                continue;
            }
            current_connections_++;
            lock.unlock();

            // 在锁外处理认证和发送状态
            {
                std::lock_guard<std::mutex> auth_lock(auth_mutex_);
                connections_without_auth_.insert({client_sock, std::chrono::steady_clock::now()});
            }
            {
                std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
                sockets_.insert(client_sock);
            }
            auth_cv_.notify_one();
            send_connection_state(ConnectionState::READY, client_sock);
        }
        else if (wait_queue_.size() < connect_wait_queue_size_)
        {
            // 连接数已满，加入等待队列
            bool was_empty = wait_queue_.empty();
            set_non_blocking(client_sock);
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
            LOG_INFO("Connection added to wait queue (current: %d, waiting: %zu)",
                     current_connections_.load(), wait_queue_.size() + 1);

            wait_queue_.push_back({client_sock,
                                   std::chrono::steady_clock::now(),
                                   client_addr});

            // 只在队列从空变非空时通知
            lock.unlock();
            if (was_empty)
            {
                wait_queue_cv_.notify_one();
            }
            send_connection_state(ConnectionState::WAITING, client_sock);
        }
        else
        {
            // 等待队列已满，拒绝连接
            lock.unlock();
            LOG_WARN("Connection rejected: both active and wait queues full");
            CLOSE_SOCKET(client_sock);
            continue;
        }
    }
#else
    // 非Linux平台（macOS、Windows）保持原有逻辑
    sockaddr_in client_addr;
#ifdef _WIN32
    int client_len = sizeof(client_addr);
#else
    socklen_t client_len = sizeof(client_addr);
#endif

    socket_t client_sock = accept(listen_socket_, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock == INVALID_SOCKET_VALUE)
    {
#ifdef _WIN32
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
        {
            LOG_ERROR("Accept error: %d", error);
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_ERROR("Accept error: %s", strerror(errno));
        }
#endif
        return;
    }

    // 尝试接受连接
    std::unique_lock<std::mutex> lock(wait_queue_mutex_);

    if (current_connections_ < max_connections_)
    {
        // 连接数未满，直接接受
        set_non_blocking(client_sock);
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
        LOG_INFO("New connection accepted: %s:%d", clientIp, ntohs(client_addr.sin_port));

        // 添加到IO复用
#ifdef __APPLE__
        struct kevent change;
        EV_SET(&change, client_sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        kevent(kqueue_fd_, &change, 1, NULL, 0, NULL);
#else // Windows
        FD_SET(client_sock, &master_set_);
#endif

        current_connections_++;
        lock.unlock();

        // 在锁外处理认证和发送状态
        {
            std::lock_guard<std::mutex> auth_lock(auth_mutex_);
            connections_without_auth_.insert({client_sock, std::chrono::steady_clock::now()});
        }
        {
            std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
            sockets_.insert(client_sock);
        }
        auth_cv_.notify_one();
        send_connection_state(ConnectionState::READY, client_sock);
    }
    else if (wait_queue_.size() < connect_wait_queue_size_)
    {
        // 连接数已满，加入等待队列
        bool was_empty = wait_queue_.empty();
        set_non_blocking(client_sock);
        LOG_INFO("Connection added to wait queue (current: %d, waiting: %zu)",
                 current_connections_.load(), wait_queue_.size() + 1);

        wait_queue_.push_back({client_sock,
                               std::chrono::steady_clock::now(),
                               client_addr});

        // 只在队列从空变非空时通知
        lock.unlock();
        if (was_empty)
        {
            wait_queue_cv_.notify_one();
        }
        send_connection_state(ConnectionState::WAITING, client_sock);
    }
    else
    {
        // 等待队列已满，拒绝连接
        lock.unlock();
        LOG_WARN("Connection rejected: both active and wait queues full");
        CLOSE_SOCKET(client_sock);
    }
#endif
}

void EyaServer::send_connection_state(ConnectionState state, socket_t client_sock)
{
    Response resp = Response::success(std::to_string(static_cast<int>(state)));
    send(resp, client_sock);
}

void EyaServer::close_socket(socket_t sock)
{
    int ret = shutdown(sock, SHUT_WR);
#ifdef _WIN32
    if (ret == SOCKET_ERROR)
    {
        LOG_ERROR("Shutdown error on fd %d: %s", sock, socket_error_to_string(errno));
    }
#else
    if (ret == -1)
    {
        LOG_ERROR("Shutdown error on fd %d: %s", sock, socket_error_to_string(errno));
    }
#endif
    CLOSE_SOCKET(sock);
#ifdef __APPLE__
#elif _WIN32
    FD_CLR(sock, &master_set_);
#elif __linux__
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock, NULL);
#endif

    if (current_connections_ > 0)
    {
        current_connections_--;
    }

    // 从未认证集合中移除
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        connections_without_auth_.erase({sock});
    }
    // 从sockets集合中移除
    {
        std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
        sockets_.erase(sock);
    }
    // 检查等待队列，激活等待的连接
    std::optional<Connection> to_activate;
    {
        std::lock_guard<std::mutex> lock(wait_queue_mutex_);
        if (!wait_queue_.empty())
        {
            to_activate = wait_queue_.front();
            wait_queue_.pop_front();
            current_connections_++;
        }
    }

    if (to_activate)
    {
        // 在锁外执行耗时操作
        set_non_blocking(to_activate->socket);
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &to_activate->client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
        LOG_INFO("Waiting connection activated: %s:%d", clientIp, ntohs(to_activate->client_addr.sin_port));

        // 添加到IO复用
#ifdef __linux__
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = to_activate->socket;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, to_activate->socket, &ev);
#elif defined(__APPLE__)
        struct kevent change;
        EV_SET(&change, to_activate->socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        kevent(kqueue_fd_, &change, 1, NULL, 0, NULL);
#else // Windows
        FD_SET(to_activate->socket, &master_set_);
#endif

        // 添加到未认证集合并通知
        {
            std::lock_guard<std::mutex> auth_lock(auth_mutex_);
            connections_without_auth_.insert({to_activate->socket, std::chrono::steady_clock::now()});
        }
        auth_cv_.notify_one();
        send_connection_state(ConnectionState::READY, to_activate->socket);
    }
}
void EyaServer::handle_request(ProtocolBody *body, socket_t client_sock)
{
    // 将请求转换为Request对象
    bool is_submitted = thread_pool_->submit([this, body, client_sock]()
                                             {
        Request *request = dynamic_cast<Request *>(body);
    if (request == nullptr)
    {
        LOG_ERROR("Transferred data is not a request");
        Response response = Response::error("Server error");
        send(response, client_sock);
        delete body;
        return;
    }
    LOG_DEBUG("Processing request from fd %d: %s",
              client_sock, request->to_string().c_str());
    Response response{0, std::monostate{}, ""};
    try
    {
        if (request->type == RequestType::AUTH)
        {
            // 处理认证请求
            LOG_DEBUG("Processing AUTH request on fd %d", client_sock);
            if (request->command == password_)
            {
                response = Response::success(auth_key_);
                // 从未认证集合中移除
                std::lock_guard<std::mutex> auth_lock(auth_mutex_);
                connections_without_auth_.erase({client_sock});
            }
            else
            {
                response = Response::error("Authentication failed");
            }
        }
        else if (request->type == RequestType::COMMAND)
        {
            // 处理命令请求
            LOG_DEBUG("Processing COMMAND request on fd %d: %s",
                      client_sock, request->command.c_str());
            if (!password_.empty() && request->auth_key != auth_key_)
            {
                response = Response::error("Authentication required");
                send(response, client_sock);
                close_socket(client_sock);
                delete body;
                return;
            }
            else
            {
                if(!RaftNode::is_init()){
                    LOG_ERROR("Raft is not initialized");
                            delete body;
                    exit(1);
                }
                static RaftNode*raft_node=RaftNode::get_instance();
                response=raft_node->submit_command(request->request_id,request->command);
            }
        }
        else
        {
            response = Response::error("Unknown request type");
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Exception while processing request on fd %d: %s",
                  client_sock, e.what());
        response = Response::error(e.what());
    }
    catch (...)
    {
        LOG_ERROR("Unknown exception while processing request on fd %d", client_sock);
        response = Response::error("Unknown server error");
    }
    // 发送响应
    send(response, client_sock);
        delete body; });
    if (!is_submitted)
    {
        LOG_ERROR("Failed to submit request to thread pool");
        Response response = Response::error("Server busy,please try again");
        send(response, client_sock);
    }
}