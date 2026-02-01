#include <string>
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>
#include <memory>
#include <functional>
#include <variant>

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

#define DEFAULT_PORT 5210
#define DEFAULT_HOST "127.0.0.1"
const size_t HEADER_SIZE = ProtocolHeader::PROTOCOL_HEADER_SIZE;

// Platform specific macros
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

// RAII Socket Wrapper
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

// WSA Guard for Windows
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

private:
    bool initialized_;
};
#endif

// Helper: Receive
Response receive_server_response(socket_t client_socket)
{
    char head_buffer[HEADER_SIZE];
    int recv_len = recv(client_socket, head_buffer, HEADER_SIZE, 0);
    if (recv_len == 0)
        throw std::runtime_error("server closed connection");
    if (recv_len == SOCKET_ERROR_VALUE)
        throw std::runtime_error("recv header failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
    if (recv_len != static_cast<int>(HEADER_SIZE))
        throw std::runtime_error("incomplete header received");

    size_t offset = 0;
    ProtocolHeader response_header;
    response_header.deserialize(head_buffer, offset);
    std::vector<char> body_buffer(response_header.length);
    if (response_header.length > 0)
    {
        recv_len = recv(client_socket, body_buffer.data(), response_header.length, 0);
        if (recv_len == SOCKET_ERROR_VALUE)
            throw std::runtime_error("recv body failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
        if (recv_len != static_cast<int>(response_header.length))
            throw std::runtime_error("incomplete body received");
    }

    offset = 0;
    Response response;
    response.deserialize(body_buffer.data(), offset);
    return response;
}

// Helper: Send
bool send_data(socket_t client_socket, const std::string &data)
{
    int total_sent = 0;
    int remaining = static_cast<int>(data.size());
    while (remaining > 0)
    {
        int sent = send(client_socket, data.c_str() + total_sent, remaining, 0);
        if (sent == 0)
        {
            std::cerr << "server closed connection" << std::endl;
            exit(0);
        }
        if (sent == SOCKET_ERROR_VALUE)
            return false;
        total_sent += sent;
        remaining -= sent;
    }
    return true;
}

// Helper: Auth
bool authenticate(SocketGuard &socket_guard, const std::string &password, std::string &auth_key)
{
    std::string data = serialize_request(RequestType::AUTH, password);
    if (!send_data(socket_guard.get(), data))
        return false;

    try
    {
        Response response = receive_server_response(socket_guard.get());
        if (response.code_ == 0)
        {
            std::cerr << "Auth failed: " << response.error_msg_ << std::endl;
            return false;
        }
        if (std::holds_alternative<std::string>(response.data_))
        {
            auth_key = std::get<std::string>(response.data_);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Auth exception: " << e.what() << std::endl;
        return false;
    }
}

// Test Runner
void run_op(socket_t sock, const std::string &auth_key, const std::string &desc, const std::string &cmd)
{
    std::cout << "[TEST] " << desc << " | Command: " << cmd << std::endl;
    std::string req = serialize_request(RequestType::COMMAND, cmd, auth_key);
    if (!send_data(sock, req))
    {
        std::cout << "-> SEND FAILED" << std::endl;
        return;
    }
    try
    {
        Response resp = receive_server_response(sock);
        std::cout << "-> " << resp.to_string() << std::endl;
    }
    catch (std::exception &e)
    {
        std::cout << "-> ERROR: " << e.what() << std::endl;
    }
    std::cout << std::endl;
}

int main(int argc, char *argv[])
{
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    std::string password = ""; // Default or prompt

    // Simple arg parsing
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            password = argv[++i];
    }

#ifdef _WIN32
    WSAInitGuard wsa;
    if (!wsa.init())
    {
        std::cerr << "WSA Init failed" << std::endl;
        return 1;
    }
#endif

    SocketGuard socket_guard;
    socket_guard.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket_guard.is_valid())
    {
        std::cerr << "Socket create failed" << std::endl;
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid address" << std::endl;
        return 1;
    }

    if (connect(socket_guard.get(), (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VALUE)
    {
        std::cerr << "Connect failed" << std::endl;
        return 1;
    }

    // Handshake/State check
    try
    {
        Response resp = receive_server_response(socket_guard.get());
        std::cout << "Initial Server State: " << resp.to_string() << std::endl;
        // If waiting, wait again (simple logic from main.cpp)
        ConnectionState state = static_cast<ConnectionState>(stoi(std::get<std::string>(resp.data_)));
        if (state == ConnectionState::WAITING)
        {
            receive_server_response(socket_guard.get());
        }
    }
    catch (...)
    {
    }

    std::string auth_key = "";
    if (!authenticate(socket_guard, password, auth_key))
    {
        return 1;
    }

    std::cout << "Authenticated. Starting Tests..." << std::endl
              << std::endl;

    // --- STRINGS ---
    run_op(socket_guard.get(), auth_key, "Set String", "set k_str v1");
    // run_op(socket_guard.get(), auth_key, "Get String", "get k_str");
    // run_op(socket_guard.get(), auth_key, "Set String Update", "set k_str v2");
    // run_op(socket_guard.get(), auth_key, "Get String Updated", "get k_str");
    run_op(socket_guard.get(), auth_key, "RemoveString", "remove k_str");

    // --- SETS ---
    run_op(socket_guard.get(), auth_key, "SAdd", "sadd k_set m1 m2 m3");
    run_op(socket_guard.get(), auth_key, "SMembers", "smembers k_set");
    run_op(socket_guard.get(), auth_key, "SRem", "srem k_set m1 m3");
    run_op(socket_guard.get(), auth_key, "SMembers After Rem", "smembers k_set");
    run_op(socket_guard.get(), auth_key, "RemoveSet", "remove k_set");

    // --- ZSETS ---
    run_op(socket_guard.get(), auth_key, "ZAdd", "zadd k_zset 10 one 20 two 30 three 40 four 50 five");
    run_op(socket_guard.get(), auth_key, "ZScore", "zscore k_zset one");
    run_op(socket_guard.get(), auth_key, "ZRank", "zrank k_zset two");
    run_op(socket_guard.get(), auth_key, "ZCard", "zcard k_zset");
    // run_op(socket_guard.get(), auth_key, "ZScore Missing", "zscore k_zset missing");
    run_op(socket_guard.get(), auth_key, "ZIncrBy", "zincr_by k_zset 5 one");
    run_op(socket_guard.get(), auth_key, "ZRangeByRank", "zrange_by_rank k_zset 0 -1");
    run_op(socket_guard.get(), auth_key, "ZRangeByScore", "zrange_by_score k_zset 15 45");
    run_op(socket_guard.get(), auth_key, "ZRem", "zrem k_zset three four");
    run_op(socket_guard.get(), auth_key, "ZRemByRank", "zrem_by_rank k_zset 0 0");     // Remove first
    run_op(socket_guard.get(), auth_key, "ZRemByScore", "zrem_by_score k_zset 40 40"); // Remove specific score
    run_op(socket_guard.get(), auth_key, "ZRangeByRank After Rem", "zrange_by_rank k_zset 0 -1");
    run_op(socket_guard.get(), auth_key, "RemoveZSet", "remove k_zset");

    // --- LISTS ---
    run_op(socket_guard.get(), auth_key, "LPush", "lpush k_list v1 v2 v3 v4 v5");
    run_op(socket_guard.get(), auth_key, "RPush", "rpush k_list v6 v7");
    run_op(socket_guard.get(), auth_key, "LRange", "lrange k_list 0 -1");
    run_op(socket_guard.get(), auth_key, "LGet", "lget k_list 0");
    run_op(socket_guard.get(), auth_key, "LSize", "lsize k_list");
    run_op(socket_guard.get(), auth_key, "LPop", "lpop k_list");
    run_op(socket_guard.get(), auth_key, "RPop", "rpop k_list");
    // "lpopp_n" and "rpopp_n" as defined in operation_type.h map
    run_op(socket_guard.get(), auth_key, "LPopN", "lpopp_n k_list 2");
    run_op(socket_guard.get(), auth_key, "RPopN", "rpopp_n k_list 1");
    run_op(socket_guard.get(), auth_key, "LRange Remainder", "lrange k_list 0 -1");
    run_op(socket_guard.get(), auth_key, "RemoveList", "remove k_list");

    // --- MAPS (HASH) ---
    run_op(socket_guard.get(), auth_key, "HSet", "hset k_hash f1 val1 f2 val2 f3 val3");
    run_op(socket_guard.get(), auth_key, "HGet", "hget k_hash f1");
    run_op(socket_guard.get(), auth_key, "HKeys", "hkeys k_hash");
    run_op(socket_guard.get(), auth_key, "HValues", "hvalues k_hash");
    run_op(socket_guard.get(), auth_key, "HEntries", "hentries k_hash");
    run_op(socket_guard.get(), auth_key, "HDel", "hdel k_hash f1 f2");
    run_op(socket_guard.get(), auth_key, "HEntries After Del", "hentries k_hash");
    run_op(socket_guard.get(), auth_key, "RemoveHash", "remove k_hash");

    // --- COMMON ---
    // run_op(socket_guard.get(), auth_key, "Exists k_str", "exists k_str");
    // run_op(socket_guard.get(), auth_key, "Exists NonExistent", "exists k_none");
    // run_op(socket_guard.get(), auth_key, "Expire k_str", "expire k_str 10");
    // run_op(socket_guard.get(), auth_key, "Remove k_str", "remove k_str");
    // run_op(socket_guard.get(), auth_key, "Exists k_str After Remove", "exists k_str");

    // Range operations are a bit more complex to interpret without knowing exact KeyRange logic,
    // but we can try a key range query if supported on keys
    // run_op(socket_guard.get(), auth_key, "Range", "range k_start k_end");

    std::cout << "Done." << std::endl;
    return 0;
}
