#include <string>
#include <iostream>
#include <cstring>
#include <limits>
#include <thread>
#include <chrono>
#include <memory>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

#include "network/protocol/protocol.h"

#define DEFAULT_PORT 5120
#define DEFAULT_HOST "127.0.0.1"
const size_t HEADER_SIZE = sizeof(Header);

// 统一平台的一些变量
#ifdef _WIN32
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define SOCKET_ERROR_VALUE SOCKET_ERROR
#define CLOSE_SOCKET closesocket
#define GET_SOCKET_ERROR WSAGetLastError
inline std::string socket_error_to_string(int error) { return std::to_string(error); }
#else
typedef int socket_t;
#define INVALID_SOCKET_VALUE -1
#define SOCKET_ERROR_VALUE -1
#define CLOSE_SOCKET close
#define GET_SOCKET_ERROR() errno
inline std::string socket_error_to_string(int error) { return strerror(error); }
#endif

// 对socket的RAII封装
class SocketGuard
{
public:
    explicit SocketGuard(socket_t sock = INVALID_SOCKET_VALUE) : socket_(sock) {}

    ~SocketGuard()
    {
        cleanup();
    }

    void reset(socket_t sock = INVALID_SOCKET_VALUE)
    {
        cleanup();
        socket_ = sock;
    }

    socket_t get() const { return socket_; }
    socket_t release()
    {
        socket_t tmp = socket_;
        socket_ = INVALID_SOCKET_VALUE;
        return tmp;
    }

    bool is_valid() const
    {
#ifdef _WIN32
        return socket_ != INVALID_SOCKET_VALUE;
#else
        return socket_ >= 0;
#endif
    }

    SocketGuard(const SocketGuard &) = delete;
    SocketGuard &operator=(const SocketGuard &) = delete;

private:
    void cleanup()
    {
        if (is_valid())
        {
            CLOSE_SOCKET(socket_);
            socket_ = INVALID_SOCKET_VALUE;
        }
    }

    socket_t socket_;
};

// 对WSA的RAII封装（Windows平台）
#ifdef _WIN32
class WSAInitGuard
{
public:
    WSAInitGuard() : initialized_(false) {}

    ~WSAInitGuard()
    {
        if (initialized_)
        {
            WSACleanup();
        }
    }

    bool init()
    {
        if (initialized_)
            return true;
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result == 0)
        {
            initialized_ = true;
        }
        return initialized_;
    }

    bool is_initialized() const { return initialized_; }

private:
    bool initialized_;
};
#endif

// 打印帮助信息
void print_usage(const char *prog_name)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "Options:\n"
            "  -p, --password <PASSWORD>  User password (required)\n"
            "  -h, --host <IP>            EyaServer IP address (default: 127.0.0.1)\n"
            "  -o, --port <NUM>           EyaServer port number (default: 5120)\n"
            "  -?, --help                 Show this help message\n",
            prog_name);
}

// Parse command line arguments
void parse_arguments(int argc, char *argv[], std::string &host, int &port, std::string &password)
{
    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "-host") == 0 || strcmp(argv[i], "-h") == 0) && i + 1 < argc)
        {
            host = argv[i + 1];
            i++;
        }
        else if ((strcmp(argv[i], "-port") == 0 || strcmp(argv[i], "-o") == 0) && i + 1 < argc)
        {
            port = atoi(argv[i + 1]);
            if (port <= 0 || port > 65535)
            {
                std::cerr << "Invalid port number: " << port << std::endl;
                exit(1);
            }
            i++;
        }
        else if (strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            exit(0);
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--password") == 0) && i + 1 < argc)
        {
            password = argv[i + 1];
            i++;
        }
        else
        {
            std::cerr << "Unrecognized argument: " << argv[i] << std::endl;
            print_usage(argv[0]);
            exit(1);
        }
    }

    if (password.empty())
    {
        std::cerr << "Error: Password is required. Use -p or --password option." << std::endl;
        print_usage(argv[0]);
        exit(1);
    }
}

// Receive server response
Response receive_server_response(socket_t client_socket)
{
    // Receive header
    char head_buffer[HEADER_SIZE];
    int recv_len = recv(client_socket, head_buffer, HEADER_SIZE, 0);
    if (recv_len == SOCKET_ERROR_VALUE)
    {
        throw std::runtime_error("recv header failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
    }
    if (recv_len != static_cast<int>(HEADER_SIZE))
    {
        throw std::runtime_error("incomplete header received");
    }

    size_t offset = 0;
    Header response_header = Header::deserialize(head_buffer, offset);

    // Receive body
    std::vector<char> body_buffer(response_header.length);
    recv_len = recv(client_socket, body_buffer.data(), response_header.length, 0);
    if (recv_len == SOCKET_ERROR_VALUE)
    {
        throw std::runtime_error("recv body failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
    }
    if (recv_len != static_cast<int>(response_header.length))
    {
        throw std::runtime_error("incomplete body received");
    }

    // Deserialize body
    offset = 0;
    return Response::deserializeResponse(body_buffer.data(), offset);
}

// Send data to server
bool send_data(socket_t client_socket, const std::string &data)
{
    int total_sent = 0;
    int remaining = static_cast<int>(data.size());

    while (remaining > 0)
    {
        int sent = send(client_socket, data.c_str() + total_sent, remaining, 0);
        if (sent == SOCKET_ERROR_VALUE)
        {
            return false;
        }
        total_sent += sent;
        remaining -= sent;
    }

    return true;
}

// Authenticate with server
bool authenticate(SocketGuard &socket_guard, const std::string &password)
{
    std::string data = serialize_request(RequestType::AUTH, password);
    if (!send_data(socket_guard.get(), data))
    {
        std::cerr << "Send auth message failed: " << socket_error_to_string(GET_SOCKET_ERROR()) << std::endl;
        return false;
    }

    try
    {
        Response response = receive_server_response(socket_guard.get());
        if (response.code_ == 0)
        {
            std::cerr << "Authentication failed: " << response.error_msg_ << std::endl;
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Receive auth response failed: " << e.what() << std::endl;
        return false;
    }
}

// Main client logic
int client_main(const std::string &host, int port, const std::string &password)
{
    SocketGuard socket_guard;

    // Create socket
    socket_guard.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket_guard.is_valid())
    {
        std::cerr << "Create socket failed: " << socket_error_to_string(GET_SOCKET_ERROR()) << std::endl;
        return 1;
    }

    // Set server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // Convert host to IP address
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid server address: " << host << std::endl;
        return 1;
    }

    // Connect to server
    if (connect(socket_guard.get(), (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VALUE)
    {
        std::cerr << "Connect failed: " << socket_error_to_string(GET_SOCKET_ERROR()) << std::endl;
        return 1;
    }

    // Authenticate
    if (!authenticate(socket_guard, password))
    {
        return 1;
    }

    std::cout << "Connected to " << host << ":" << port << std::endl;
    std::cout << "Type 'exit' to quit" << std::endl;

    // Command loop
    while (true)
    {
        std::cout << "> ";
        std::string command;
        std::getline(std::cin, command);

        if (command.empty())
        {
            continue;
        }

        if (command == "exit" || command == "quit")
        {
            break;
        }

        std::string req_data = serialize_request(RequestType::COMMAND, command);
        if (!send_data(socket_guard.get(), req_data))
        {
            std::cerr << "Send command failed: " << socket_error_to_string(GET_SOCKET_ERROR()) << std::endl;
            return 1;
        }

        try
        {
            Response response = receive_server_response(socket_guard.get());
            std::cout << response.to_string() << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Receive command response failed: " << e.what() << std::endl;
            return 1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    std::string password;

    // Parse command line arguments
    parse_arguments(argc, argv, host, port, password);

    // Initialize socket library (Windows only)
#ifdef _WIN32
    WSAInitGuard wsa_guard;
    if (!wsa_guard.init())
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    // Run client
    return client_main(host, port, password);
}