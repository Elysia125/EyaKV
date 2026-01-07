#include "network/server.h"
#include "logger/logger.h"
#include "network/protocol/protocol.h"
#include <iostream>
#include <cstring>
#include "storage/storage.h"
#include "common/utils.h"
#include "common/operation_type.h"
#define HEADER_SIZE sizeof(Header)
#define HEADER_SIZE_LIMIT 1024 * 1024
#ifdef __linux__
#define INITIAL_BUFFER_SIZE 8096
#endif
EyaServer::EyaServer(Storage *storage, const std::string &ip,
                     const u_short port,
                     const std::string &password,
                     const uint32_t max_connections,
                     const uint32_t connect_wait_queue_size,
                     const uint32_t connect_wait_timeout,
                     const uint32_t worker_thread_count,
                     const uint32_t worker_queue_size,
                     const uint32_t worker_wait_timeout)
    : ip_(ip),
      port_(port),
      password_(password),
      max_connections_(max_connections),
      connect_wait_queue_size_(connect_wait_queue_size),
      connect_wait_timeout_(connect_wait_timeout),
      worker_thread_count_(worker_thread_count),
      worker_queue_size_(worker_queue_size),
      worker_wait_timeout_(worker_wait_timeout),
      listen_socket_(INVALID_SOCKET_VALUE),
      is_running_(false),
      current_connections_(0),
      storage_(storage)
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
    auth_key_ = generate_general_key(32);
}

EyaServer::~EyaServer()
{
    if (listen_socket_ != INVALID_SOCKET_VALUE)
    {
        CLOSE_SOCKET(listen_socket_);
    }

#ifdef _WIN32
    WSACleanup();
#elif defined(__linux__)
    if (epoll_fd_ != -1)
        close(epoll_fd_);
    delete[] events_;
#elif defined(__APPLE__)
    if (kqueue_fd_ != -1)
        close(kqueue_fd_);
    delete[] event_list_;
#endif
}

void EyaServer::set_non_blocking(socket_t sock)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool EyaServer::start()
{
    // 1. 创建Socket
    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ == INVALID_SOCKET_VALUE)
    {
        LOG_ERROR("Socket creation failed");
        return false;
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
        LOG_ERROR("Bind failed on %s:%d", ip_.c_str(), port_);
        return false;
    }

    // 4. 监听
    if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR_VALUE)
    {
        LOG_ERROR("Listen failed");
        return false;
    }

    // 5. 设置非阻塞并初始化IO复用
    set_non_blocking(listen_socket_);
    LOG_INFO("EyaServer started on %s:%d", ip_.c_str(), port_);

#ifdef __linux__
    // Linux epoll初始化（边缘触发模式）
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1)
    {
        LOG_ERROR("Epoll create failed: %s", strerror(errno));
        return false;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET; // 边缘触发模式
    ev.data.fd = listen_socket_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_socket_, &ev) == -1)
    {
        LOG_ERROR("Epoll control failed: %s", strerror(errno));
        return false;
    }
#elif defined(__APPLE__)
    // macOS kqueue初始化
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ == -1)
    {
        LOG_ERROR("Kqueue create failed: %s", strerror(errno));
        return false;
    }
    struct kevent change;
    EV_SET(&change, listen_socket_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kqueue_fd_, &change, 1, NULL, 0, NULL) == -1)
    {
        LOG_ERROR("Kevent failed: %s", strerror(errno));
        return false;
    }
#else
    // Windows select初始化
    FD_SET(listen_socket_, &master_set_);
#endif

    // 6. 初始化线程池
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
        return false;
    }

    is_running_ = true;
    return true;
}

void EyaServer::run()
{
    while (is_running_)
    {
#ifdef __linux__
        // LINUX (epoll)
        int nfds = epoll_wait(epoll_fd_, events_, max_connections_, -1);
        if (nfds == -1)
        {
            LOG_ERROR("Epoll wait error: %s", strerror(errno));
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
        // macOS (kqueue)
        int nev = kevent(kqueue_fd_, NULL, 0, event_list_, max_connections_, NULL);
        if (nev == -1)
        {
            LOG_ERROR("Kqueue wait error: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nev; ++i)
        {
            int fd = (int)event_list_[i].ident;
            if (event_list_[i].flags & EV_EOF)
            {
                // 客户端断开
                LOG_INFO("Client disconnected (EOF), fd: %d", fd);
                CLOSE_SOCKET(fd);
                current_connections_--;
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
        fd_set readSet = master_set_; // select会修改集合，需要拷贝
        int activity = select(0, &readSet, NULL, NULL, NULL);

        if (activity == SOCKET_ERROR_VALUE)
        {
            LOG_ERROR("Select error: %d", WSAGetLastError());
            break;
        }

        // 遍历所有可能的socket
        for (unsigned int i = 0; i < master_set_.fd_count; i++)
        {
            socket_t sock = master_set_.fd_array[i];
            if (FD_ISSET(sock, &readSet))
            {
                if (sock == listen_socket_)
                {
                    handle_accept();
                }
                else
                {
                    handle_client(sock);
                }
            }
        }
#endif
    }
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

        // 检查连接数限制
        if (current_connections_ >= max_connections_)
        {
            LOG_WARN("Connection limit reached (%d), rejecting new connection", max_connections_);
            CLOSE_SOCKET(client_sock);
            continue;
        }

        set_non_blocking(client_sock);

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
        LOG_INFO("New connection: %s:%d", clientIp, ntohs(client_addr.sin_port));

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

    // 检查连接数限制
    if (current_connections_ >= max_connections_)
    {
        LOG_WARN("Connection limit reached (%d), rejecting new connection", max_connections_);
        CLOSE_SOCKET(client_sock);
        return;
    }

    set_non_blocking(client_sock);

    char clientIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
    LOG_INFO("New connection: %s:%d", clientIp, ntohs(client_addr.sin_port));

#ifdef __APPLE__
    struct kevent change;
    EV_SET(&change, client_sock, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kqueue_fd_, &change, 1, NULL, 0, NULL) == -1)
    {
        LOG_ERROR("Kevent failed for client socket: %s", strerror(errno));
        CLOSE_SOCKET(client_sock);
        return;
    }
#else // Windows
    FD_SET(client_sock, &master_set_);
#endif
    current_connections_++;
#endif
}

void EyaServer::handle_client(socket_t client_sock)
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
                LOG_ERROR("Recv buffer overflow on fd %d", client_sock);
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
                break; // 数据已全部读取完毕
            }
            LOG_ERROR("Recv error on fd %d: %s", client_sock, strerror(errno));
            goto cleanup;
        }
        else if (bytes_received == 0)
        {
            // 对方关闭连接
            LOG_INFO("Client disconnected, fd: %d", client_sock);
            goto cleanup;
        }

        total_received += bytes_received;

        // 处理已接收的数据
        size_t processed = 0;
        while (processed + HEADER_SIZE <= total_received)
        {
            // 解析头部
            size_t offset = processed;
            Header header = Header::deserialize(recv_buffer.data() + processed, offset);

            if (header.length > HEADER_SIZE_LIMIT)
            {
                LOG_ERROR("Invalid body length on fd %d: %zu (max: %d)",
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
                Request request = Request::deserializeRequest(
                    recv_buffer.data() + body_offset, offset);

                // 将请求处理任务提交到线程池
                // 捕获client_sock副本以供线程池使用
                bool submitted = thread_pool_->submit(
                    [this, &request, client_sock]()
                    {
                        this->handle_request(request, client_sock);
                    });

                if (!submitted)
                {
                    LOG_WARN("Failed to submit task to ThreadPool for fd %d, rejecting",
                             client_sock);
                    // 返回错误信息
                    Response response = Response::error("Server is busy, please try again later");
                    send_response(response, client_sock);
                }
                else
                {
                    LOG_DEBUG("Task submitted to ThreadPool for fd %d", client_sock);
                }
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Error processing request on fd %d: %s", client_sock, e.what());
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
        LOG_WARN("Unprocessed data left on fd %d: %zu bytes", client_sock, total_received);
    }

    close_socket(client_sock);

#else
    // 非Linux平台(macOS、Windows)
    char header_buffer[HEADER_SIZE];
    memset(header_buffer, 0, HEADER_SIZE);

    int bytes_received = recv(client_sock, header_buffer, HEADER_SIZE, 0);

    if (bytes_received <= 0)
    {
        // 断开连接或出错
        if (bytes_received == 0)
        {
            LOG_INFO("Client disconnected, fd: %d", client_sock);
        }
        else
        {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
            {
                LOG_ERROR("Recv error on fd %d: %d", client_sock, error);
            }
            else
            {
                return; // 非阻塞模式下需要稍后重试
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("Recv error on fd %d: %s", client_sock, strerror(errno));
            }
            else
            {
                return; // 非阻塞模式下需要稍后重试
            }
#endif
        }

        close_socket(client_sock);
        return;
    }

    // 验证头部接收是否完整
    if (bytes_received < HEADER_SIZE)
    {
        LOG_WARN("Incomplete header received on fd %d (got %d bytes, expected %zu)",
                 client_sock, bytes_received, HEADER_SIZE);
        return;
    }

    try
    {
        size_t offset = 0;
        Header header = Header::deserialize(header_buffer, offset);

        // 防止过大分配导致栈溢出
        if (header.length > HEADER_SIZE_LIMIT) // 限制为1MB
        {
            LOG_ERROR("Invalid body length on fd %d: %zu (max: %d)",
                      client_sock, header.length, HEADER_SIZE_LIMIT);
            close_socket(client_sock);
            return;
        }

        // 动态分配body buffer，避免栈溢出
        char *body_buffer = new char[header.length];

        int body_bytes = recv(client_sock, body_buffer, header.length, 0);
        if (body_bytes <= 0)
        {
            if (body_bytes == 0)
            {
                LOG_INFO("Client disconnected during body recv, fd: %d", client_sock);
            }
            else
            {
#ifdef _WIN32
                int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK)
                {
                    LOG_ERROR("Recv body error on fd %d: %d", client_sock, error);
                }
                else
                {
                    delete[] body_buffer;
                    return; // 非阻塞模式，稍后重试
                }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    LOG_ERROR("Recv body error on fd %d: %s", client_sock, strerror(errno));
                }
                else
                {
                    delete[] body_buffer;
                    return; // 非阻塞模式，稍后重试
                }
#endif
            }

            delete[] body_buffer;
            close_socket(client_sock);
            return;
        }

        if (static_cast<size_t>(body_bytes) < header.length)
        {
            LOG_WARN("Incomplete body received on fd %d (got %d bytes, expected %zu)",
                     client_sock, body_bytes, header.length);
            delete[] body_buffer;
            return;
        }

        offset = 0;
        Request request = Request::deserializeRequest(body_buffer, offset);
        delete[] body_buffer;

        // 将请求处理任务提交到线程池
        // 捕获client_sock副本以供线程池使用
        bool submitted = thread_pool_->submit(
            [this, &request, client_sock]()
            {
                this->handle_request(request, client_sock);
            });

        if (!submitted)
        {
            LOG_WARN("Failed to submit task to ThreadPool for fd %d, rejecting",
                     client_sock);
            // 返回错误信息
            Response response = Response::error("Server is busy, please try again later");
            send_response(response, client_sock);
        }
        else
        {
            LOG_DEBUG("Task submitted to ThreadPool for fd %d", client_sock);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error processing request on fd %d: %s", client_sock, e.what());
        close_socket(client_sock);
    }
#endif
}

void EyaServer::close_socket(socket_t sock)
{
    CLOSE_SOCKET(sock);
#ifdef __APPLE__
#elif _WIN32
    FD_CLR(sock, &master_set_);
#elif __linux__
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock, NULL);
#endif
    if (current_connections_ > 0)
        current_connections_--;
}
void EyaServer::handle_request(const Request &request, socket_t client_sock)
{
    LOG_DEBUG("Processing request from fd %d: %s",
              client_sock, request.to_string().c_str());
    Response response{0, std::monostate{}, ""};
    try
    {
        if (request.type == RequestType::AUTH)
        {
            // 处理认证请求
            LOG_DEBUG("Processing AUTH request on fd %d", client_sock);
            if (request.command == password_)
            {
                response = Response::success(auth_key_);
            }
            else
            {
                response = Response::error("Authentication failed");
            }
        }
        else if (request.type == RequestType::COMMAND)
        {
            // 处理命令请求
            LOG_DEBUG("Processing COMMAND request on fd %d: %s",
                      client_sock, request.command.c_str());
            if (request.auth_key != auth_key_)
            {
                response = Response::error("Authentication required");
                send_response(response, client_sock);
                close_socket(client_sock);
                return;
            }
            else
            {
                // 解析命令并执行
                std::vector<std::string> command_parts = split(request.command, ' ');
                if (command_parts.empty())
                {
                    response = Response::error("Invalid command");
                }
                else
                {
                    uint8_t operation = stringToOperationType(command_parts[0]);
                    command_parts.erase(command_parts.begin());
                    response = storage_->execute(operation, command_parts);
                }
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
    send_response(response, client_sock);
}
void EyaServer::send_response(const Response &response, socket_t client_sock)
{
    std::string response_data = serialize_response(response);

    // 发送响应
    int sent_bytes = send(client_sock, response_data.data(),
                          static_cast<int>(response_data.size()), 0);
    if (sent_bytes < 0)
    {
#ifdef _WIN32
        int error = WSAGetLastError();
        LOG_ERROR("Send error on fd %d: %d", client_sock, error);
#else
        LOG_ERROR("Send error on fd %d: %s", client_sock, strerror(errno));
#endif
    }
    else if (static_cast<size_t>(sent_bytes) != response_data.size())
    {
        LOG_WARN("Incomplete send on fd %d: sent %d/%zu bytes",
                 client_sock, sent_bytes, response_data.size());
    }
    else
    {
        LOG_DEBUG("Response sent successfully to fd %d (%d bytes)",
                  client_sock, sent_bytes);
    }
}