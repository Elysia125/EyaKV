#ifndef RAFT_CLIENT_H
#define RAFT_CLIENT_H
#include "common/socket/socket.h"

class RaftClient
{
private:
    std::string host_;
    int port_;
    SocketGuard socket_guard_;
#ifdef _WIN32
    WSAInitGuard wsa_guard_;
#endif
public:
    RaftClient(std::string host, int port) : host_(host), port_(port), socket_guard_(), wsa_guard_()
    {
        if (!wsa_guard_.init())
        {
            throw std::runtime_error("Failed to initialize Winsock");
        }
    }
    ~RaftClient() = default;

    void connect()
    {
        socket_guard_.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!socket_guard_.is_valid())
        {
            throw std::runtime_error("Failed to create socket: " + socket_error_to_string(GET_SOCKET_ERROR()));
        }

        // Set server address
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);

        // Convert host to IP address
        if (inet_pton(AF_INET, host_.c_str(), &server_addr.sin_addr) <= 0)
        {
            throw std::runtime_error("Invalid server address: " + host_);
        }

        // Connect to server
        if (::connect(socket_guard_.get(), (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VALUE)
        {
            throw std::runtime_error("Connect failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
        }
    }

    inline int send_request(const std::string &data)
    {
        return send_data(socket_guard_.get(), data);
    }

    inline int receive_response(std::string &data, uint32_t data_size, uint32_t timeout = 0)
    {
        return receive_data(socket_guard_.get(), data, data_size, timeout);
    }
}
#endif // RAFT_CLIENT_H