#pragma once

#include <asio.hpp>
#include <memory>
#include <array>
#include "storage/storage.h"
#include "protocol/codec.h"

namespace tinykv::network {

/**
 * @brief Connection 负责处理单个客户端连接的读写和协议解析。
 * 
 * 继承 enable_shared_from_this 以便在异步回调中保持对象存活。
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(asio::ip::tcp::socket socket, tinykv::storage::Storage* storage);

    void Start();

private:
    void DoReadLength();
    void DoReadBody(std::size_t length);
    void ProcessRequest(const tinykv::protocol::Message& req);
    void SendResponse(const tinykv::protocol::Message& resp);
    void DoWrite();

    asio::ip::tcp::socket socket_;
    tinykv::storage::Storage* storage_;

    // Buffer for reading length prefix (4 bytes)
    uint32_t network_length_;
    
    // Buffer for reading body
    std::vector<char> buffer_;
};

} // namespace tinykv::network