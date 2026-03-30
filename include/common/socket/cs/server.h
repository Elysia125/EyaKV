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
#include <shared_mutex>
// IO复用技术头文件
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#define HEADER_SIZE ProtocolHeader::PROTOCOL_HEADER_SIZE
#define HEADER_SIZE_LIMIT 1024 * 1024

// 引入应用层读写缓冲区 (TcpSession) 解决 EAGAIN 阻塞死锁
struct TcpSession
{
    socket_t socket;                                  // 客户端 socket 描述符
    sockaddr_in client_addr;                          // 客户端地址信息
    std::vector<char> recv_buffer;                    // 未处理数据缓冲区，接收时使用
    std::string send_buffer;                          // 未发送数据缓冲区，异步发送时使用
    std::mutex write_mutex;                           // 保护 send_buffer 的互斥锁
    int slave_index;                                  // 所属 Slave 线程索引
    std::chrono::steady_clock::time_point start_time; // 连接开始时间，用于超时管理
    std::atomic<bool> close_after_send{false};        // 标记是否在发送完毕后关闭

    TcpSession(socket_t sock, sockaddr_in addr, int s_idx)
        : socket(sock), client_addr(addr), slave_index(s_idx)
    {
        start_time = std::chrono::steady_clock::now();
    }
};
using TcpSessionPtr = std::shared_ptr<TcpSession>;

// 2. 封装底层多路复用 (消灭宏泛滥)
struct EventLoop
{
#ifdef __linux__
    int epoll_fd = -1;
    struct epoll_event *events = nullptr;
#elif defined(__APPLE__)
    int kqueue_fd = -1;
    struct kevent *events = nullptr;
#else
    fd_set read_set;
    fd_set write_set;
    std::mutex set_mutex;
#endif

    void init(int max_conn)
    {
#ifdef __linux__
        epoll_fd = epoll_create1(0);
        events = new epoll_event[max_conn];
#elif defined(__APPLE__)
        kqueue_fd = kqueue();
        events = new kevent[max_conn];
#else
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
#endif
    }

    ~EventLoop()
    {
#ifdef __linux__
        if (epoll_fd != -1)
            close(epoll_fd);
        delete[] events;
#elif defined(__APPLE__)
        if (kqueue_fd != -1)
            close(kqueue_fd);
        delete[] events;
#endif
    }

    bool add_socket(socket_t sock)
    {
#ifdef __linux__
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = sock;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) == -1)
        {
            LOG_ERROR("Epoll ctl failed for client socket: {}", strerror(errno));
            return false;
        }
#elif defined(__APPLE__)
        struct kevent change;
        EV_SET(&change, sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        kevent(kqueue_fd, &change, 1, NULL, 0, NULL);
#else
        std::lock_guard<std::mutex> lock(set_mutex);
        FD_SET(sock, &read_set);
        LOG_INFO("Added socket {} to Windows read set,has {} sockets", sock, read_set.fd_count);
        // 检查是否接近 FD_SETSIZE 限制
        uint32_t total_sockets = read_set.fd_count + 1; // +1 for listen_socket
        if (total_sockets >= FD_SETSIZE - 10)
        {
            LOG_WARN("Approaching FD_SETSIZE limit: {} sockets (FD_SETSIZE={})",
                     total_sockets, FD_SETSIZE);
            LOG_WARN("Consider increasing FD_SETSIZE in socket.h or using WSAPoll instead of select");
        }
#endif
        return true;
    }
    /**
     * @brief 设置 socket 的写事件监听状态
     * @param sock 要设置的 socket 描述符
     * @param enable 是否启用写事件监听，true 表示启用，false 表示禁用
     */
    void set_write(socket_t sock, bool enable)
    {
#ifdef __linux__
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | (enable ? EPOLLOUT : 0);
        ev.data.fd = sock;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock, &ev);
#elif defined(__APPLE__)
        struct kevent change;
        EV_SET(&change, sock, EVFILT_WRITE, enable ? (EV_ADD | EV_ENABLE) : (EV_DELETE | EV_DISABLE), 0, 0, NULL);
        kevent(kqueue_fd, &change, 1, NULL, 0, NULL);
#else
        std::lock_guard<std::mutex> lock(set_mutex);
        if (enable)
            FD_SET(sock, &write_set);
        else
            FD_CLR(sock, &write_set);
#endif
    }

    void remove_socket(socket_t sock)
    {
#ifdef __linux__
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock, NULL);
#elif defined(__APPLE__)
        struct kevent change[2];
        EV_SET(&change[0], sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&change[1], sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(kqueue_fd, change, 2, NULL, 0, NULL);
#else
        std::lock_guard<std::mutex> lock(set_mutex);
        FD_CLR(sock, &read_set);
        FD_CLR(sock, &write_set);
#endif
    }
};

class TCPServer : public TCPBase
{
protected:
    socket_t listen_socket_;                 // 监听socket描述符
    std::atomic<bool> is_running_;           // 服务器运行状态标志
    const uint32_t max_connections_;         // 最大并发连接数
    const uint32_t connect_wait_queue_size_; // 等待队列最大容量
    const uint32_t connect_wait_timeout_;    // 连接等待超时时间（秒）
    std::atomic_uint current_connections_;   // 当前连接数
    // O(1) Session 管理
    std::unordered_map<socket_t, TcpSessionPtr> all_sessions_;
    std::shared_mutex sessions_mutex_;
    /**
     * 等待队列管理
     */
    std::deque<Connection> wait_queue_;          // 等待队列，存储等待连接资源的客户端
    std::mutex wait_queue_mutex_;                // 等待队列互斥锁
    std::condition_variable_any wait_queue_cv_;  // 条件变量，用于通知监控线程
    std::thread queue_monitor_thread_;           // 等待队列监控线程，检查连接超时
    std::atomic<bool> wait_thread_stop_monitor_; // 监控线程停止标志
    /*
     * 主从 Reactor 资源
     */
    EventLoop master_loop_;
    std::vector<std::unique_ptr<EventLoop>> slave_loops_;
    std::vector<std::thread> slave_threads_;
    uint32_t next_slave_ = 0;
    static constexpr size_t INITIAL_BUFFER_SIZE = 4096;

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

    virtual void handle_request(ProtocolBody *body, socket_t client_sock, const sockaddr_in &client_addr = sockaddr_in()) = 0;

    /**
     * @brief 封装分配 Slave 的方法
     *
     */
    bool assign_to_slave(socket_t client_sock, const sockaddr_in &client_addr)
    {
        int slave_idx = next_slave_ % slave_loops_.size();
        next_slave_++;
        auto session = std::make_shared<TcpSession>(client_sock, client_addr, slave_idx);
        {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            all_sessions_[client_sock] = session;
        }

        // 失败时需要回滚字典
        if (!slave_loops_[slave_idx]->add_socket(client_sock))
        {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            all_sessions_.erase(client_sock);
            return false;
        }
        LOG_INFO("Assigned client socket {} to slave loop {}", client_sock, slave_idx);
        return true;
    }
    /**
     * @brief 从 Slave 中移除连接并关闭 Socket
     * 这个方法会先从 all_sessions_ 中找到对应的 session，获取它所属的 slave_index，
     */
    void remove_from_slave(socket_t sock)
    {
        TcpSessionPtr session;
        {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            auto it = all_sessions_.find(sock);
            if (it != all_sessions_.end())
            {
                session = it->second;
                all_sessions_.erase(it);
            }
        }
        if (session)
        {
            LOG_INFO("Removing socket {} from slave loop {}", sock, session->slave_index);
            slave_loops_[session->slave_index]->remove_socket(sock);
        }
        // CLOSE_SOCKET(sock);
    }

    // 异步非阻塞发送
    void async_send(socket_t client_sock, const std::string &data)
    {
        TcpSessionPtr session;
        {
            std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
            auto it = all_sessions_.find(client_sock);
            if (it != all_sessions_.end())
            {
                session = it->second;
            }
        }
        if (!session)
            return;

        std::lock_guard<std::mutex> lock(session->write_mutex);
        session->send_buffer += data;
        flush_send_buffer(session);
    }

    // 优雅关闭：发送完指定的响应后，自动关闭连接
    void send_and_close(const ProtocolBody &body, socket_t socket)
    {
        std::string sbody = body.serialize();
        std::string request = build_header(sbody.length()) + sbody;

        TcpSessionPtr session;
        {
            std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
            auto it = all_sessions_.find(socket);
            if (it != all_sessions_.end())
                session = it->second;
        }
        if (!session)
            return;

        std::lock_guard<std::mutex> lock(session->write_mutex);
        session->send_buffer += request;
        session->close_after_send = true; // 标记发完即关
        flush_send_buffer(session);
    }

    void flush_send_buffer(TcpSessionPtr session)
    {
        if (session->send_buffer.empty())
        {
            if (session->close_after_send)
                close_socket(session->socket);
            return;
        }

        // 必须循环发送，直到触发 EAGAIN
        while (!session->send_buffer.empty())
        {
            int sent = ::send(session->socket, session->send_buffer.data(), session->send_buffer.size(), 0);
            if (sent > 0)
            {
                LOG_INFO("Sent {} bytes to socket {}, remaining {} bytes", sent, session->socket, session->send_buffer.size() - sent);
                session->send_buffer.erase(0, sent);
            }
            else if (sent == SOCKET_ERROR_VALUE)
            {
                int err = GET_SOCKET_ERROR();
#ifdef _WIN32
                if (err == WSAEWOULDBLOCK)
                    break;
#else
                if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
                    break;
#endif
                return; // 发生真实网络错误直接返回
            }
            else
            {
                break; // 对方断开
            }
        }

        // 判断：如果发空了且标记了发完即关，就直接关闭；否则注册 EPOLLOUT
        if (session->send_buffer.empty() && session->close_after_send)
        {
            close_socket(session->socket);
        }
        else if (!session->send_buffer.empty())
        {
            slave_loops_[session->slave_index]->set_write(session->socket, true);
        }
        else
        {
            slave_loops_[session->slave_index]->set_write(session->socket, false);
        }
    }

    // 重写基类 send 以接管所有外层调用
    int send(const ProtocolBody &body, socket_t socket) override
    {
        std::string sbody = body.serialize();
        std::string request = build_header(sbody.length()) + sbody;
        async_send(socket, request);
        return 0;
    }
    int send(const std::string &body, socket_t socket) override
    {
        std::string request = build_header(body.length()) + body;
        async_send(socket, request);
        return 0;
    }

    virtual void on_wait_queue_activated(socket_t sock, const sockaddr_in &addr) {}

public:
    TCPServer(const std::string &ip, const u_short port, const uint32_t max_connections, const uint32_t connect_wait_queue_size, const uint32_t connect_wait_timeout)
        : TCPBase(ip, port), max_connections_(max_connections), connect_wait_queue_size_(connect_wait_queue_size), connect_wait_timeout_(connect_wait_timeout), current_connections_(0), is_running_(false), wait_thread_stop_monitor_(false), listen_socket_(INVALID_SOCKET_VALUE)
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
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

        // 启动 Master Loop
        master_loop_.init(max_connections_);
        master_loop_.add_socket(listen_socket_);

        // 启动 Slave Loops
        uint32_t slave_count = std::thread::hardware_concurrency();
        if (slave_count == 0)
        {
            slave_count = 4;
        }
        for (uint32_t i = 0; i < slave_count; ++i)
        {
            slave_loops_.push_back(std::make_unique<EventLoop>());
            slave_loops_.back()->init(max_connections_);
        }
        is_running_.store(true, std::memory_order_release);
        for (uint32_t i = 0; i < slave_count; ++i)
        {
            slave_threads_.push_back(std::thread(&TCPServer::run_slave, this, i));
        }
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
        // is_running_.store(true, std::memory_order_relaxed);
    }

    void run()
    {
        LOG_INFO("TCPServer master loop started");
        while (is_running_.load(std::memory_order_relaxed))
        {
#ifdef __linux__
            // LINUX (epoll) - 设置 100ms 超时，代替 -1（无限阻塞）
            int nfds = epoll_wait(master_loop_.epoll_fd, master_loop_.events, 1, 100);
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
                if (master_loop_.events[i].data.fd == listen_socket_)
                {
                    handle_accept();
                }
            }

#elif defined(__APPLE__)
            // macOS (kqueue) - 设置 100ms 超时，代替 NULL
            struct timespec ts = {0, 100000000};
            int nev = kevent(master_loop_.kqueue_fd, NULL, 0, master_loop_.events, 1, &ts);
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
                if (((int)master_loop_.events[i].ident) == listen_socket_)
                {
                    handle_accept();
                }
            }

#else
            // Windows (select)
            fd_set read_set;
            {
                std::lock_guard<std::mutex> l(master_loop_.set_mutex);
                read_set = master_loop_.read_set;
            }

            // Windows 下如果传入空的 fd_set，select 会立刻报错 10022 (WSAEINVAL)。
            // 为了防止 CPU 100% 空转，当没有连接且退出前，我们让它 sleep 一下。
            if (read_set.fd_count == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 设置 100ms 超时，代替 NULL（无限阻塞）
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100,000 微秒 = 100ms

            int activity = select(0, &read_set, NULL, NULL, &timeout);

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

            if (FD_ISSET(listen_socket_, &read_set))
            {
                handle_accept();
            }
#endif
        }

        LOG_INFO("TCPServer run master loop gracefully exited.");
    }

    void run_slave(int slave_index)
    {
        LOG_INFO("TCPServer slave loop {} started.", slave_index);
        EventLoop *loop = slave_loops_[slave_index].get();
        while (is_running_.load())
        {
#ifdef __linux__
            int nfds = epoll_wait(loop->epoll_fd, loop->events, max_connections_, 10);

            if (nfds == -1)
            {
                if (errno == EINTR)
                    continue; // 被系统信号打断是正常的，继续循环
                if (!is_running_.load(std::memory_order_relaxed))
                    break; // 如果已经要求停止，直接退出
                LOG_ERROR("epoll_wait error: {}", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; i++)
            {
                if (loop->events[i].events & EPOLLIN)
                {
                    //LOG_INFO("Handling read event for client: {}", loop->events[i].data.fd);
                    handle_client_read(loop->events[i].data.fd);
                }
                if (loop->events[i].events & EPOLLOUT)
                {
                    //LOG_INFO("Handling write event for client: {}", loop->events[i].data.fd);
                    handle_client_write(loop->events[i].data.fd);
                }
            }
#elif defined(__APPLE__)
            struct timespec ts = {0, 10000000};
            int nev = kevent(loop->kqueue_fd, NULL, 0, loop->events, max_connections_, &ts);
            if (nev == -1)
            {
                if (errno == EINTR)
                    continue;
                if (!is_running_.load(std::memory_order_relaxed))
                    break;
                LOG_ERROR("kevent error: {}", strerror(errno));
                break;
            }
            for (int i = 0; i < nev; i++)
            {
                int fd = loop->events[i].ident;
                if (loop->events[i].flags & EV_EOF)
                    close_socket(fd);
                else
                {
                    if (loop->events[i].filter == EVFILT_READ)
                    {
                        LOG_INFO("Handling read event for client: {}", fd);
                        handle_client_read(fd);
                    }
                    if (loop->events[i].filter == EVFILT_WRITE)
                    {
                        LOG_INFO("Handling write event for client: {}", fd);
                        handle_client_write(fd);
                    }
                }
            }
#else
            fd_set r_set, w_set;
            {
                std::lock_guard<std::mutex> l(loop->set_mutex);
                r_set = loop->read_set;
                w_set = loop->write_set;
            }
            if (r_set.fd_count == 0 && w_set.fd_count == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            LOG_INFO("Slave loop {} has {} read events and {} write events.", slave_index, r_set.fd_count, w_set.fd_count);
            struct timeval tv = {0, 10000};
            // 如果 fd_set 为空，强制传递 nullptr，否则 Windows 会直接崩溃返回 WSAEINVAL！
            fd_set *p_rset = (r_set.fd_count > 0) ? &r_set : nullptr;
            fd_set *p_wset = (w_set.fd_count > 0) ? &w_set : nullptr;
            int activity = select(0, p_rset, p_wset, NULL, &tv);
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
            else if (activity > 0)
            {
                for (u_int i = 0; i < r_set.fd_count; i++)
                {
                    LOG_INFO("Handling read event for client: {}", r_set.fd_array[i]);
                    handle_client_read(r_set.fd_array[i]);
                }
                for (u_int i = 0; i < w_set.fd_count; i++)
                {
                    LOG_INFO("Handling write event for client: {}", w_set.fd_array[i]);
                    handle_client_write(w_set.fd_array[i]);
                }
            }
#endif
        }
    }

    void handle_client_read(socket_t client_sock)
    {
        TcpSessionPtr session;
        {
            std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
            auto it = all_sessions_.find(client_sock);
            if (it != all_sessions_.end())
                session = it->second;
        }
        if (!session)
            return;

        auto &recv_buffer = session->recv_buffer;
        while (true)
        {
            char buf[8192];
            int bytes = ::recv(client_sock, buf, sizeof(buf), 0);
            if (bytes > 0)
            {
                recv_buffer.insert(recv_buffer.end(), buf, buf + bytes);
            }
            else if (bytes == 0)
            {
                LOG_INFO("Client socket closed: {}", client_sock);
                close_socket(client_sock);
                return;
            }
            else
            {
                int err = GET_SOCKET_ERROR();
#ifdef _WIN32
                if (err == WSAEWOULDBLOCK)
                    break;
#else
                if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
                    break;
#endif
                LOG_ERROR("Recv error on socket {}: {} - {}", client_sock, err, socket_error_to_string(err));
                close_socket(client_sock);
                return;
            }
        }
        LOG_INFO("Received {} bytes from client {}.", recv_buffer.size(), client_sock);
        size_t processed = 0;
        while (processed + HEADER_SIZE <= recv_buffer.size())
        {
            try
            {
                size_t offset = 0;
                ProtocolHeader header = new_header();
                header.deserialize(recv_buffer.data() + processed, offset);
                if (header.length > HEADER_SIZE_LIMIT || processed + HEADER_SIZE + header.length > recv_buffer.size())
                {
                    LOG_WARN("Received malformed packet from client {}: header length {} exceeds limit {}", client_sock, header.length, HEADER_SIZE_LIMIT);
                    break;
                }

                ProtocolBody *body = new_body();
                offset = 0;
                body->deserialize(recv_buffer.data() + processed + HEADER_SIZE, offset);
                handle_request(body, client_sock);
                processed += HEADER_SIZE + header.length;
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Malformed packet dropped on fd {}: {}", client_sock, e.what());
                // 遇到脏数据，断开恶意连接，防止它持续发脏数据
                close_socket(client_sock);
                return;
            }
        }

        if (processed > 0)
        {
            recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + processed);
        }
    }

    void handle_client_write(socket_t client_sock)
    {
        TcpSessionPtr session;
        {
            std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
            auto it = all_sessions_.find(client_sock);
            if (it != all_sessions_.end())
                session = it->second;
        }
        if (session)
        {
            std::lock_guard<std::mutex> lock(session->write_mutex);
            flush_send_buffer(session);
        }
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
        for (auto &t : slave_threads_)
            if (t.joinable())
                t.join();
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
        std::vector<socket_t> to_close;
        {
            std::shared_lock<std::shared_mutex> lock(sessions_mutex_);
            for (auto &pair : all_sessions_)
                to_close.push_back(pair.first);
        }
        for (auto sock : to_close)
        {
            close_socket(sock);
        }
        LOG_INFO("Listen socket and clients closed.");

        if (listen_socket_ != INVALID_SOCKET_VALUE)
        {
            CLOSE_SOCKET(listen_socket_);
            listen_socket_ = INVALID_SOCKET_VALUE; // 修复：置为无效值
        }
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

    virtual void add_new_connection(socket_t &client_sock, const sockaddr_in &client_addr)
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
            if (!assign_to_slave(client_sock, client_addr))
            {
                LOG_ERROR("Failed to assign client socket to slave: {}", client_sock);
                CLOSE_SOCKET(client_sock);
                lock.unlock();
                return;
            }
            LOG_INFO("New connection accepted: {}:{}", clientIp, ntohs(client_addr.sin_port));
            current_connections_++;
            lock.unlock();
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
            // assign_to_slave(client_sock, client_addr);
        }
        else
        {
            // 等待队列已满，拒绝连接
            lock.unlock();
            LOG_WARN("Connection rejected: both active and wait queues full");
            CLOSE_SOCKET(client_sock);
            client_sock = INVALID_SOCKET_VALUE; // 置为无效值
        }
    }
    /**
     *  @brief 处理客户端请求的核心方法(已废除)
     */
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
        LOG_INFO("Closing socket {}", sock);
        remove_from_slave(sock);
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

        if (current_connections_ > 0)
        {
            current_connections_.fetch_sub(1, std::memory_order_relaxed);
        }
        // 检查等待队列，激活等待的连接
        std::optional<Connection> to_activate;
        {
            std::lock_guard<std::mutex> lock(wait_queue_mutex_);
            if (!wait_queue_.empty())
            {
                to_activate = wait_queue_.front();
                wait_queue_.pop_front();
            }
        }

        if (to_activate.has_value())
        {
            // 在锁外执行耗时操作
            set_non_blocking(to_activate->socket);
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &to_activate->client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);

            LOG_INFO("Activating connection from wait queue: {}:{}, current active: {}, waiting: {}",
                     clientIp, ntohs(to_activate->client_addr.sin_port),
                     current_connections_.load(), wait_queue_.size());
            if (!assign_to_slave(to_activate->socket, to_activate->client_addr))
            {
                LOG_ERROR("Failed to assign activated client socket to slave: {}", to_activate->socket);
                CLOSE_SOCKET(to_activate->socket);
            }
            else
            {
                current_connections_++;
                on_wait_queue_activated(to_activate->socket, to_activate->client_addr);
            }
        }
    }
};

#endif