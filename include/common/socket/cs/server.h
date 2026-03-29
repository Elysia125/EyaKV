#ifndef SERVER_H_
#define SERVER_H_

#include "common/socket/cs/common.h"
#include "logger/logger.h"
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <optional>
#include <fcntl.h>
// IO复用技术头文件
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#define HEADER_SIZE ProtocolHeader::PROTOCOL_HEADER_SIZE
#define HEADER_SIZE_LIMIT 1024 * 1024

class TCPServer : public TCPBase
{
protected:
    socket_t listen_socket_;                 // 监听socket描述符
    std::atomic<bool> is_running_;           // 服务器运行状态标志
    const uint32_t max_connections_;         // 最大并发连接数
    const uint32_t connect_wait_queue_size_; // 等待队列最大容量
    const uint32_t connect_wait_timeout_;    // 连接等待超时时间（秒）
    std::atomic_uint current_connections_;   // 当前连接数
    std::unordered_set<socket_t> sockets_;   // 所有连接的socket集合
    std::mutex sockets_mutex_;               // socket集合互斥锁
    /**
     * 等待队列管理
     */
    std::deque<Connection> wait_queue_;          // 等待队列，存储等待连接资源的客户端
    std::mutex wait_queue_mutex_;                // 等待队列互斥锁
    std::condition_variable_any wait_queue_cv_;  // 条件变量，用于通知监控线程
    std::thread queue_monitor_thread_;           // 等待队列监控线程，检查连接超时
    std::atomic<bool> wait_thread_stop_monitor_; // 监控线程停止标志

    static constexpr size_t INITIAL_BUFFER_SIZE = 4096;
    /**
     * 平台特定的IO复用句柄或数据
     */
#ifdef __linux__
    int epoll_fd_;               // epoll 文件描述符
    struct epoll_event *events_; // epoll 事件数组，用于存储就绪事件
#elif defined(__APPLE__)
    int kqueue_fd_;             // kqueue 文件描述符
    struct kevent *event_list_; // kevent 事件列表，用于存储就绪事件
#else // Windows (使用select模型)
    fd_set master_set_; // select 主集合，存储所有需要监听的 socket
#endif

    /**
     * @brief 设置 socket 为非阻塞模式
     *
     * 非阻塞模式下的 socket 操作不会阻塞调用线程，
     * 如果没有数据可读写，会立即返回错误。
     * 这对于 IO 复用（epoll/kqueue/select）是必需的。
     *
     * @param sock 要设置的 socket 描述符
     */
    void set_non_blocking(socket_t sock)
    {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    virtual ProtocolBody *new_body() = 0;
    virtual ProtocolHeader new_header()
    {
        return ProtocolHeader();
    }

    virtual void handle_request(ProtocolBody *body, socket_t client_sock) = 0;

public:
    TCPServer(const std::string &ip, const u_short port, const uint32_t max_connections, const uint32_t connect_wait_queue_size, const uint32_t connect_wait_timeout)
        : TCPBase(ip, port), max_connections_(max_connections), connect_wait_queue_size_(connect_wait_queue_size), connect_wait_timeout_(connect_wait_timeout), current_connections_(0), is_running_(false), wait_thread_stop_monitor_(false), listen_socket_(INVALID_SOCKET_VALUE)
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        FD_ZERO(&master_set_);
#endif
    }

    ~TCPServer()
    {
        stop();
    }

    void start()
    {
        // 1. 创建Socket
        listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_socket_ == INVALID_SOCKET_VALUE)
        {
            throw std::runtime_error("Failed to create socket");
        }
        // 2. 设置端口复用
        int opt = 1;
#ifdef _WIN32
        setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
        setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        // 3. 绑定
        sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port_);
        inet_pton(AF_INET, ip_.c_str(), &serverAddr.sin_addr);

        if (bind(listen_socket_, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR_VALUE)
        {
            throw std::runtime_error("Failed to bind socket on " + ip_ + ":" + std::to_string(port_));
        }
        // 4. 监听
        if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR_VALUE)
        {
            throw std::runtime_error("Failed to listen on socket");
        }

        // 5. 设置非阻塞并初始化IO复用
        set_non_blocking(listen_socket_);

#ifdef __linux__
        // Linux epoll初始化（边缘触发模式）
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ == -1)
        {
            throw std::runtime_error("Failed to create epoll");
        }
        events_ = new epoll_event[max_connections_];
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
        ev.data.fd = listen_socket_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_socket_, &ev) == -1)
        {
            throw std::runtime_error("Failed to add listen socket to epoll");
        }
#elif defined(__APPLE__)
        // macOS kqueue初始化
        kqueue_fd_ = kqueue();
        if (kqueue_fd_ == -1)
        {
            throw std::runtime_error("Failed to create kqueue");
        }
        event_list_ = new kevent[max_connections_];
        struct kevent change;
        EV_SET(&change, listen_socket_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        if (kevent(kqueue_fd_, &change, 1, NULL, 0, NULL) == -1)
        {
            throw std::runtime_error("Failed to add listen socket to kqueue");
        }
#else
        // Windows select初始化
        FD_SET(listen_socket_, &master_set_);
#endif
        // 启动等待队列监控线程
        queue_monitor_thread_ = std::thread([this]()
                                            {
                        while (!wait_thread_stop_monitor_.load(std::memory_order_relaxed)) {
                            std::unique_lock<std::mutex> lock(wait_queue_mutex_);

                            // 计算最早连接的超时剩余时间
                            std::chrono::milliseconds wait_time = std::chrono::seconds(1);
                            if (!wait_queue_.empty()) {
                                auto now = std::chrono::steady_clock::now();
                                auto& front = wait_queue_.front();
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - front.start_time);
                                auto timeout = std::chrono::milliseconds(connect_wait_timeout_ * 1000) - elapsed;
                                wait_time = std::min(wait_time, std::max(timeout, std::chrono::milliseconds(0)));
                            }

                            // 只等待停止信号或超时
                            wait_queue_cv_.wait_for(lock, wait_time, [this]() {
                                return wait_thread_stop_monitor_.load(std::memory_order_relaxed);
                            });
                            if (wait_thread_stop_monitor_.load(std::memory_order_relaxed)) {
                                break;
                            }

                            // 检查并移除超时的连接
                            auto now = std::chrono::steady_clock::now();
                            auto timeout_duration = std::chrono::seconds(connect_wait_timeout_);

                            while (!wait_queue_.empty()) {
                                auto& waiting = wait_queue_.front();
                                if (now - waiting.start_time >= timeout_duration) {
                                    // 超时，关闭连接
                                    char clientIp[INET_ADDRSTRLEN];
                                    inet_ntop(AF_INET, &waiting.client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
                                    CLOSE_SOCKET(waiting.socket);
                                    wait_queue_.pop_front();
                                } else {
                                    break;  // 后续连接未超时
                                }
                            }

                            lock.unlock();
                        } });
        is_running_.store(true, std::memory_order_relaxed);
    }

    void run()
    {
        while (is_running_.load(std::memory_order_relaxed))
        {
#ifdef __linux__
            // LINUX (epoll) - 设置 100ms 超时，代替 -1（无限阻塞）
            int nfds = epoll_wait(epoll_fd_, events_, max_connections_, 100);
            if (nfds == -1)
            {
                if (errno == EINTR)
                    continue; // 被系统信号打断是正常的，继续循环
                if (!is_running_.load(std::memory_order_relaxed))
                    break; // 如果已经要求停止，直接退出
                LOG_ERROR("epoll_wait error: {}", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i)
            {
                if (events_[i].data.fd == listen_socket_)
                {
                    handle_accept();
                }
                else
                {
                    handle_client(events_[i].data.fd);
                }
            }

#elif defined(__APPLE__)
            // macOS (kqueue) - 设置 100ms 超时，代替 NULL
            struct timespec timeout;
            timeout.tv_sec = 0;
            timeout.tv_nsec = 100 * 1000000; // 100ms
            int nev = kevent(kqueue_fd_, NULL, 0, event_list_, max_connections_, &timeout);
            if (nev == -1)
            {
                if (errno == EINTR)
                    continue;
                if (!is_running_.load(std::memory_order_relaxed))
                    break;
                LOG_ERROR("kevent error: {}", strerror(errno));
                break;
            }
            for (int i = 0; i < nev; ++i)
            {
                int fd = (int)event_list_[i].ident;
                if (event_list_[i].flags & EV_EOF)
                {
                    // 客户端断开
                    close_socket(fd);
                }
                else if (fd == listen_socket_)
                {
                    handle_accept();
                }
                else
                {
                    handle_client(fd);
                }
            }

#else
            // Windows (select)
            fd_set readSet = master_set_;

            // Windows 下如果传入空的 fd_set，select 会立刻报错 10022 (WSAEINVAL)。
            // 为了防止 CPU 100% 空转，当没有连接且退出前，我们让它 sleep 一下。
            if (readSet.fd_count == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 设置 100ms 超时，代替 NULL（无限阻塞）
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100,000 微秒 = 100ms

            int activity = select(0, &readSet, NULL, NULL, &timeout);

            if (activity == SOCKET_ERROR_VALUE)
            {
                int error = GET_SOCKET_ERROR();
                // 如果是因为 stop() 中途关闭了 socket 导致的错误，正常退出
                if (!is_running_.load(std::memory_order_relaxed))
                {
                    break;
                }
                LOG_ERROR("select error: {} - {}", error, socket_error_to_string(error).c_str());
                if (error == 10038)
                {
                    LOG_ERROR("select failed: possibly too many sockets for FD_SETSIZE={}", FD_SETSIZE);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 避免无限报错引发死循环
                continue;
            }

            if (activity == 0)
            {
                // 每 100ms 没有消息就会来到这里。
                // 此时循环继续，回到 while 条件判断 is_running_ 是否为 false，实现优雅退出。
                continue;
            }

            // Windows 下直接遍历 readSet.fd_array 性能更高，无需遍历全局集合再 FD_ISSET
            for (uint32_t i = 0; i < readSet.fd_count; i++)
            {
                socket_t sock = readSet.fd_array[i];
                if (sock == listen_socket_)
                {
                    handle_accept();
                }
                else
                {
                    handle_client(sock);
                }
            }
#endif
        }

        LOG_INFO("TCPServer run loop gracefully exited."); // 加上这句日志，用于验证修复效果
    }

    virtual void stop()
    {
        // 1. 使用 CAS 保证 stop 逻辑只会被执行一次，完美解决被多次调用的问题
        bool expected = true;
        if (!is_running_.compare_exchange_strong(expected, false, std::memory_order_relaxed))
        {
            return; // 如果已经是 false，说明已经 stop 过了，直接返回
        }

        // 停止等待队列监控线程
        LOG_INFO("Stopping queue monitor thread...");
        wait_thread_stop_monitor_.store(true, std::memory_order_relaxed);
        wait_queue_cv_.notify_all();
        if (queue_monitor_thread_.joinable())
        {
            queue_monitor_thread_.join();
        }

        // 清理等待队列中的所有连接
        LOG_INFO("Cleaning up wait queue...");
        {
            std::unique_lock<std::mutex> lock(wait_queue_mutex_);
            while (!wait_queue_.empty())
            {
                CLOSE_SOCKET(wait_queue_.front().socket);
                wait_queue_.pop_front();
            }
        }
        LOG_INFO("Wait queue cleaned up.");

        // 关闭监听Socket和所有的客户端Socket
        {
            LOG_INFO("Closing listen socket and all clients...");
            std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
            for (auto &socket : sockets_)
            {
                CLOSE_SOCKET(socket);
            }
            sockets_.clear(); // 修复：关闭后必须清空集合
        }
        LOG_INFO("Listen socket and clients closed.");

        if (listen_socket_ != INVALID_SOCKET_VALUE)
        {
            CLOSE_SOCKET(listen_socket_);
            listen_socket_ = INVALID_SOCKET_VALUE; // 修复：置为无效值
        }

#ifdef _WIN32
        WSACleanup();
        LOG_INFO("Windows socket cleanup completed.");
#elif defined(__linux__)
        if (epoll_fd_ != -1)
        {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        if (events_ != nullptr)
        {
            delete[] events_;
            events_ = nullptr;
        }
        LOG_INFO("Linux socket cleanup completed.");
#elif defined(__APPLE__)
        if (kqueue_fd_ != -1)
        {
            close(kqueue_fd_);
            kqueue_fd_ = -1;
        }
        if (event_list_ != nullptr)
        {
            delete[] event_list_;
            event_list_ = nullptr;
        }
        LOG_INFO("macOS socket cleanup completed.");
#endif

        LOG_INFO("TCP server stopped.");
    }

    virtual void handle_accept()
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
                LOG_ERROR("Accept failed: {}", strerror(errno));
            }

            add_new_connection(client_sock, client_addr);
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
                LOG_ERROR("Accept error: {}", error);
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("Accept error: {}", strerror(errno));
            }
#endif
            return;
        }

        // 尝试接受连接
        add_new_connection(client_sock, client_addr);
#endif
    }

    virtual void add_new_connection(socket_t client_sock, const sockaddr_in &client_addr)
    {
        // 尝试接受连接
        std::unique_lock<std::mutex> lock(wait_queue_mutex_);

        if (current_connections_ < max_connections_)
        {
            // 连接数未满，直接接受
            set_non_blocking(client_sock);
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);

            // 添加到IO复用
            add_socket_to_epoll(client_sock);
            LOG_INFO("New connection accepted: {}:{}", clientIp, ntohs(client_addr.sin_port));

            current_connections_++;
            lock.unlock();
            {
                std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
                sockets_.insert(client_sock);
            }
        }
        else if (wait_queue_.size() < connect_wait_queue_size_)
        {
            // 连接数已满，加入等待队列
            bool was_empty = wait_queue_.empty();
            set_non_blocking(client_sock);
            LOG_INFO("Connection added to wait queue (current: {}, waiting: {})",
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
            {
                std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
                sockets_.insert(client_sock);
            }
        }
        else
        {
            // 等待队列已满，拒绝连接
            lock.unlock();
            LOG_WARN("Connection rejected: both active and wait queues full");
            CLOSE_SOCKET(client_sock);
        }
    }

    void add_socket_to_epoll(socket_t client_sock)
    {
        // 添加到IO复用
#ifdef __APPLE__
        struct kevent change;
        EV_SET(&change, client_sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        kevent(kqueue_fd_, &change, 1, NULL, 0, NULL);
#elif __linux__
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
        ev.data.fd = client_sock;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_sock, &ev) == -1)
        {
            LOG_ERROR("Epoll ctl failed for client socket: {}", strerror(errno));
            close_socket(client_sock);
        }
#else // Windows
        FD_SET(client_sock, &master_set_);

        // 检查是否接近 FD_SETSIZE 限制
        uint32_t total_sockets = master_set_.fd_count + 1; // +1 for listen_socket
        if (total_sockets >= FD_SETSIZE - 10)
        {
            LOG_WARN("Approaching FD_SETSIZE limit: {} sockets (FD_SETSIZE={})",
                     total_sockets, FD_SETSIZE);
            LOG_WARN("Consider increasing FD_SETSIZE in socket.h or using WSAPoll instead of select");
        }
#endif
    }

    virtual void handle_client(socket_t client_sock)
    {
#ifdef __linux__
        // 边缘触发模式：必须循环读取直到 EAGAIN
        std::vector<char> recv_buffer;
        recv_buffer.resize(INITIAL_BUFFER_SIZE);
        size_t total_received = 0;

        while (true)
        {
            // 确保缓冲区足够大
            if (total_received + 4096 > recv_buffer.size())
            {
                if (recv_buffer.size() * 2 > HEADER_SIZE_LIMIT)
                {
                    LOG_ERROR("Recv buffer overflow on fd {}", client_sock);
                    goto cleanup;
                }
                recv_buffer.resize(recv_buffer.size() * 2);
            }

            int bytes_received = recv(client_sock, recv_buffer.data() + total_received,
                                      recv_buffer.size() - total_received, 0);

            if (bytes_received < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    return; // 数据已全部读取完毕
                }
                LOG_ERROR("Recv error on fd {}: {}", client_sock, socket_error_to_string(GET_SOCKET_ERROR()).c_str());
                goto cleanup;
            }
            else if (bytes_received == 0)
            {
                // 对方关闭连接
                LOG_INFO("Client disconnected, fd: {}", client_sock);
                goto cleanup;
            }

            total_received += bytes_received;

            // 处理已接收的数据
            size_t processed = 0;
            while (processed + HEADER_SIZE <= total_received)
            {
                // 解析头部
                size_t offset = processed;
                ProtocolHeader header = new_header();
                header.deserialize(recv_buffer.data() + processed, offset);

                if (header.length > HEADER_SIZE_LIMIT)
                {
                    LOG_ERROR("Invalid body length on fd {}: {} (max: {})",
                              client_sock, header.length, HEADER_SIZE_LIMIT);
                    goto cleanup;
                }

                // 检查body是否完整
                if (processed + HEADER_SIZE + header.length > total_received)
                {
                    break; // 数据不完整，等待更多数据
                }

                // 处理完整的消息
                try
                {
                    size_t body_offset = processed + HEADER_SIZE;
                    offset = 0;
                    ProtocolBody *body = new_body();
                    body->deserialize(recv_buffer.data() + body_offset, offset);

                    // 处理请求，由子类决定何时释放 body
                    handle_request(body, client_sock);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR("Error processing request on fd {}: {}", client_sock, e.what());
                    goto cleanup;
                }

                processed += HEADER_SIZE + header.length;
            }

            // 移动未处理的数据到缓冲区开头
            if (processed > 0 && processed < total_received)
            {
                memmove(recv_buffer.data(), recv_buffer.data() + processed,
                        total_received - processed);
                total_received -= processed;
            }
            else if (processed == total_received)
            {
                total_received = 0;
            }
        }

    cleanup:
        if (total_received > 0)
        {
            LOG_WARN("Unprocessed data left on fd {}: {} bytes", client_sock, total_received);
        }

        close_socket(client_sock);

#else
        // 非Linux平台(macOS、Windows)
        try
        {
            ProtocolBody *body = new_body();
            int bytes_received = receive(*body, client_sock);
            if (bytes_received < 0)
            {
                if (bytes_received == -1)
                {
                    LOG_ERROR("Recv error on fd {}: timeout", client_sock);
                    close_socket(client_sock);
                }
                else if (bytes_received == -2)
                {
                    LOG_ERROR("fd closed {}", client_sock);
                    close_socket(client_sock);
                }
                else
                {
                    LOG_ERROR("Recv error on fd {}: {}", client_sock, socket_error_to_string(bytes_received).c_str());
                    close_socket(client_sock);
                }
            }
            else if (bytes_received != 0)
            {
                // 处理请求，由子类决定何时释放 body
                handle_request(body, client_sock);
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Error processing request on fd {}: {}", client_sock, e.what());
            close_socket(client_sock);
        }
#endif
    }

    virtual void close_socket(socket_t sock)
    {
        int ret = shutdown(sock, SHUT_WR);
#ifdef _WIN32
        if (ret == SOCKET_ERROR)
        {
            LOG_ERROR("Shutdown error on fd {}: {}", sock, socket_error_to_string(GET_SOCKET_ERROR()).c_str());
        }
#else
        if (ret == -1)
        {
            LOG_ERROR("Shutdown error on fd {}: {}", sock, socket_error_to_string(GET_SOCKET_ERROR()).c_str());
        }
#endif
        CLOSE_SOCKET(sock);
#ifdef __APPLE__
#elif defined(_WIN32)
        FD_CLR(sock, &master_set_);
#elif __linux__
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock, NULL);
#endif

        if (current_connections_ > 0)
        {
            current_connections_--;
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

        if (to_activate.has_value())
        {
            // 在锁外执行耗时操作
            set_non_blocking(to_activate->socket);
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &to_activate->client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);

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
        }
    }
};

#endif