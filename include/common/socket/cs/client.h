#ifndef SOCKET_CS_CLIENT_H_
#define SOCKET_CS_CLIENT_H_
#include "common/socket/cs/common.h"

class TCPClient : public TCPBase
{
private:
    SocketGuard socket_guard_;
#ifdef _WIN32
    WSAInitGuard wsa_guard_;
#endif
public:
    TCPClient(std::string host, u_short port) : TCPBase(host, port), socket_guard_(), wsa_guard_()
    {
        if (!wsa_guard_.init())
        {
            throw std::runtime_error("Failed to initialize Winsock");
        }
    }
    ~TCPClient()
    {
        if (socket_guard_.is_valid())
        {
            CLOSE_SOCKET(socket_guard_.get());
        }
    }

    socket_t get_socket() const
    {
        return socket_guard_.get();
    }

    bool is_connected() const
    {
        return socket_guard_.is_valid();
    }

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
        if (inet_pton(AF_INET, ip_.c_str(), &server_addr.sin_addr) <= 0)
        {
            throw std::runtime_error("Invalid server address: " + ip_);
        }

        // Connect to server
        if (::connect(socket_guard_.get(), (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VALUE)
        {
            throw std::runtime_error("Connect failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
        }
    }

    int send(const ProtocolBody &body)
    {
        return TCPBase::send(body, socket_guard_.get());
    }
    int receive(ProtocolBody &body, uint32_t timeout = 0)
    {
        int ret = TCPBase::receive(body, socket_guard_.get(), timeout);
        if (ret == -2)
        {
            socket_guard_.reset(INVALID_SOCKET_VALUE);
        }
        return ret;
    }
};

#endif