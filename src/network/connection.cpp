#include "network/connection.h"
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
Connection::Connection(asio::ip::tcp::socket socket, Storage *storage)
        : socket_(std::move(socket)), storage_(storage)
    {
    }

    void Connection::Start()
    {
        DoReadLength();
    }

    void Connection::DoReadLength()
    {
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(&network_length_, 4),
                         [this, self](std::error_code ec, std::size_t /*length*/)
                         {
                             if (!ec)
                             {
                                 // Convert from network byte order (big endian) to host
                                 uint32_t body_length = ntohl(network_length_);
                                 if (body_length > 10 * 1024 * 1024)
                                 { // 10MB limit check
                                     std::cerr << "Request too large: " << body_length << std::endl;
                                     return;
                                 }
                                 DoReadBody(body_length);
                             }
                         });
    }

    void Connection::DoReadBody(std::size_t length)
    {
        buffer_.resize(length);
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(buffer_),
                         [this, self](std::error_code ec, std::size_t /*length*/)
                         {
                             if (!ec)
                             {
                                 eyakv::protocol::Message req;
                                 if (eyakv::protocol::Message::Decode(buffer_, req))
                                 {
                                     ProcessRequest(req);
                                 }
                                 else
                                 {
                                     std::cerr << "Failed to parse Request." << std::endl;
                                 }
                             }
                         });
    }

    void Connection::ProcessRequest(const eyakv::protocol::Message &req)
    {
        eyakv::protocol::Message resp;
        resp.type = eyakv::protocol::MessageType::kResponse;

        switch (req.type)
        {
        case eyakv::protocol::MessageType::kGet:
        {
            auto val = storage_->get(req.key);
            if (val.has_value())
            {
                //resp.value = val.value();
                resp.status = true;
            }
            else
            {
                resp.status = false;
                resp.error = "Key not found";
            }
            break;
        }
        case eyakv::protocol::MessageType::kPut:
        {
            bool success = storage_->put(req.key, req.value);
            resp.status = success;
            break;
        }
        case eyakv::protocol::MessageType::kDelete:
        {
            bool success = storage_->remove(req.key);
            resp.status = success;
            break;
        }
        default:
        {
            resp.status = false;
            resp.error = "Unknown request type";
            break;
        }
        }

        SendResponse(resp);
    }

    void Connection::SendResponse(const eyakv::protocol::Message &resp)
    {
        auto self(shared_from_this());

        // Serialize
        std::vector<char> body = resp.Encode();

        // Prefix length
        uint32_t len = static_cast<uint32_t>(body.size());
        uint32_t net_len = htonl(len);

        // Combine into one buffer
        auto write_buffer = std::make_shared<std::vector<char>>();
        write_buffer->resize(4 + len);
        std::memcpy(write_buffer->data(), &net_len, 4);
        std::memcpy(write_buffer->data() + 4, body.data(), len);

        asio::async_write(socket_, asio::buffer(*write_buffer),
                          [this, self, write_buffer](std::error_code ec, std::size_t /*length*/)
                          {
                              if (!ec)
                              {
                                  // Keep reading next request (Keep-Alive)
                                  DoReadLength();
                              }
                          });
    }
