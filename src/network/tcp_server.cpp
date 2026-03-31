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
    // 1. 先初始化所有前置资源（如线程池），不要提前启动底层 Server！
    ThreadPool::Config pool_config{
        worker_thread_count_,       // 工作线程数量
        worker_queue_size_,         // 任务队列大小
        worker_wait_timeout_ * 1000 // 等待超时时间（毫秒）
    };
    try
    {
        thread_pool_ = std::make_unique<ThreadPool>(pool_config);
        LOG_INFO("ThreadPool initialized with {} threads", worker_thread_count_);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to initialize ThreadPool: {}", e.what());
        throw std::runtime_error("Failed to initialize ThreadPool:" + std::string(e.what()));
    }

    // 2. 前置资源就绪后，再启动底层的 Reactor 线程引擎！
    TCPServer::start();
    // 启动认证线程
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
                        LOG_INFO("❌ 客户端 {} 超过 2 秒未发送 AUTH 认证，触发超时踢出！", it->socket);
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
    is_running_ = true;
}

void EyaServer::handle_accept()
{
    // 边缘触发模式下需要循环 accept 直到返回 EAGAIN
    while (true)
    {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        socket_t client_sock = accept(listen_socket_, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET_VALUE)
        {
            int err = GET_SOCKET_ERROR();
#ifdef _WIN32
            if (err == WSAEWOULDBLOCK)
                break;
#else
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
                break;
#endif
            LOG_ERROR("Accept error: {}", socket_error_to_string(err));
            break; // 没有新连接了，跳出循环
        }

        // 检查连接数限制和等待队列
        std::unique_lock<std::mutex> lock(wait_queue_mutex_);

        if (current_connections_ < max_connections_)
        {
            // 连接数未满，直接接受
            set_non_blocking(client_sock);

            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
            LOG_INFO("New connection accepted: {}:{}", clientIp, ntohs(client_addr.sin_port));

            if (assign_to_slave(client_sock, client_addr))
            {
                LOG_INFO("Client socket {} assigned to slave successfully", client_sock);
                current_connections_++;
            }
            else
            {
                LOG_ERROR("Failed to assign client socket to slave: {}", client_sock);
                CLOSE_SOCKET(client_sock);
                lock.unlock();
                continue;
            }
            lock.unlock();

            // 在锁外处理认证和发送状态
            {
                std::lock_guard<std::mutex> auth_lock(auth_mutex_);
                connections_without_auth_.insert({client_sock, std::chrono::steady_clock::now()});
            }
            auth_cv_.notify_one();
            send_connection_state(ConnectionState::READY, client_sock);
            LOG_INFO("Connection accepted and ready: {}:{}", clientIp, ntohs(client_addr.sin_port));
        }
        else if (wait_queue_.size() < connect_wait_queue_size_)
        {
            // 连接数已满，加入等待队列
            bool was_empty = wait_queue_.empty();
            set_non_blocking(client_sock);
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
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
}

void EyaServer::send_connection_state(ConnectionState state, socket_t client_sock)
{
    Response resp = Response::success(std::to_string(static_cast<int>(state)));
    TCPBase::send(resp, client_sock);
}

void EyaServer::close_socket(socket_t sock)
{
    // 1. 清理 EyaServer 派生类特有的状态
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        connections_without_auth_.erase({sock});
    }

    // 2. 其它底层释放、唤醒等待队列等复杂操作，全权交给基类！
    TCPServer::close_socket(sock);
}
void EyaServer::on_wait_queue_activated(socket_t sock, const sockaddr_in &addr)
{
    // 基类已经把它放入 Reactor，这里只管特有业务：添加未认证集合并发送 READY
    {
        std::lock_guard<std::mutex> auth_lock(auth_mutex_);
        connections_without_auth_.insert({sock, std::chrono::steady_clock::now()});
    }
    auth_cv_.notify_one();
    send_connection_state(ConnectionState::READY, sock);
}
void EyaServer::handle_request(ProtocolBody *body, socket_t client_sock, const sockaddr_in &client_addr)
{
    LOG_INFO("Handling request from client: {}", client_sock);
    std::shared_ptr<ProtocolBody> safe_body(body);
    // 将请求转换为Request对象
    bool is_submitted = thread_pool_->submit([this, safe_body, client_sock]()
                                             {
        //std::unique_ptr<ProtocolBody> safe_body(body); 
        Request *request = dynamic_cast<Request *>(safe_body.get());
    if (request == nullptr)
    {
        LOG_ERROR("Transferred data is not a request");
        Response response = Response::error("Server error");
        send(response, client_sock);
        return;
    }
    LOG_DEBUG("Processing request from fd {}: {}",
              client_sock, request->to_string().c_str());
    Response response{0, std::monostate{}, "",""};
    try
    {
        if (request->type == RequestType::AUTH)
        {
            // 处理认证请求
            LOG_DEBUG("Processing AUTH request on fd {}", client_sock);
            if (request->password == password_)
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
        else if (request->type == RequestType::COMMAND||request->type==RequestType::BATCH_COMMAND)
        {
            // 处理命令请求
            LOG_DEBUG("Processing COMMAND request on fd {}: {}",
                      client_sock, request->command.c_str());
            if (!password_.empty() && request->auth_key != auth_key_)
            {
                response = Response::error("Authentication required");
                send(response, client_sock);
                close_socket(client_sock);
                return;
            }
            else
            {
                if(!RaftNode::is_init()){
                    LOG_ERROR("Raft is not initialized");
                    exit(1);
                }
                static RaftNode*raft_node=RaftNode::get_instance();
                if(request->type == RequestType::COMMAND){
                    response=raft_node->submit_command(request->id,request->command);
                }else{
                    auto batch_responses=raft_node->submit_batch_command(request->commands);
                    send(serialize({request->id,batch_responses}),client_sock);
                    return;
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
        LOG_ERROR("Exception while processing request on fd {}: {}",
                  client_sock, e.what());
        response = Response::error(e.what());
    }
    catch (...)
    {
        LOG_ERROR("Unknown exception while processing request on fd {}", client_sock);
        response = Response::error("Unknown server error");
    }
    // 发送响应
    send(response, client_sock); });
    if (!is_submitted)
    {
        LOG_ERROR("Failed to submit request to thread pool");
        Response response = Response::error("Server busy,please try again");
        send(response, client_sock);
    }
}