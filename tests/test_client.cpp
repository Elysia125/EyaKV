#include <iostream>
#include <asio.hpp>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include "protocol/codec.h"
#include "storage/storage.h"
#include "network/server.h"

// Simple synchronous client for testing
class TinyKVClient {
public:
    TinyKVClient(const std::string& host, unsigned short port)
        : socket_(io_context_) {
        asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(host), port);
        socket_.connect(endpoint);
    }

    tinykv::protocol::Message SendRequest(const tinykv::protocol::Message& req) {
        // Serialize
        std::vector<char> body = req.Encode();
        
        uint32_t len = htonl(static_cast<uint32_t>(body.size()));
        
        std::vector<asio::const_buffer> buffers;
        buffers.push_back(asio::buffer(&len, 4));
        buffers.push_back(asio::buffer(body));

        asio::write(socket_, buffers);

        // Read Response
        uint32_t resp_len_net;
        asio::read(socket_, asio::buffer(&resp_len_net, 4));
        uint32_t resp_len = ntohl(resp_len_net);

        std::vector<char> resp_buf(resp_len);
        asio::read(socket_, asio::buffer(resp_buf));

        tinykv::protocol::Message resp;
        tinykv::protocol::Message::Decode(resp_buf, resp);
        return resp;
    }

private:
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
};

TEST(NetworkTest, ClientServerIntegration) {
    std::string data_dir = "test_data_network";
    unsigned short port = 8081;

    // Ensure clean state (Try once, ignore error)
    if (std::filesystem::exists(data_dir)) {
        try {
            std::filesystem::remove_all(data_dir);
        } catch (...) {
            std::cerr << "Warning: Setup cleanup failed, proceeding..." << std::endl;
        }
    }

    {
        tinykv::storage::Storage storage(data_dir);
        tinykv::network::Server server(&storage, port);

        std::thread server_thread([&server]() {
            server.Run();
        });

        // Give server a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        try {
            TinyKVClient client("127.0.0.1", port);

            // Test PUT
            {
                tinykv::protocol::Message req;
                req.type = tinykv::protocol::MessageType::kPut;
                req.key = "net_key";
                req.value = "net_val";
                auto resp = client.SendRequest(req);
                EXPECT_TRUE(resp.status);
            }

            // Test GET
            {
                tinykv::protocol::Message req;
                req.type = tinykv::protocol::MessageType::kGet;
                req.key = "net_key";
                auto resp = client.SendRequest(req);
                EXPECT_TRUE(resp.status);
                EXPECT_EQ(resp.value, "net_val");
            }

            // Test DELETE
            {
                tinykv::protocol::Message req;
                req.type = tinykv::protocol::MessageType::kDelete;
                req.key = "net_key";
                auto resp = client.SendRequest(req);
                EXPECT_TRUE(resp.status);
            }

             // Test GET (After Delete)
            {
                tinykv::protocol::Message req;
                req.type = tinykv::protocol::MessageType::kGet;
                req.key = "net_key";
                auto resp = client.SendRequest(req);
                EXPECT_FALSE(resp.status);
            }

        } catch (const std::exception& e) {
            server.Stop();
            server_thread.join();
            FAIL() << "Client exception: " << e.what();
        }

        server.Stop();
        server_thread.join();
    } 

    // Skip cleanup at end to avoid flakes on Windows
    // if (std::filesystem::exists(data_dir)) { ... }
}
