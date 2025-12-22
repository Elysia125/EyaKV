#pragma once

#include <asio.hpp>
#include <memory>
#include <thread>
#include <vector>
#include "storage/storage.h"

class Connection; // Forward declaration

/**
 * @brief Server 负责监听端口并接受连接 (Reactor 模式/Asio).
 */
class Server
{
public:
    Server(Storage *storage, unsigned short port);
    ~Server();

    /**
     * @brief 启动服务器（阻塞）。
     */
    void Run();

    /**
     * @brief 停止服务器。
     */
    void Stop();

private:
    void DoAccept();

    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    Storage *storage_; // 引用，不拥有所有权
};