#include <string>
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>
#include <memory>
#include <functional>
#include <variant>
#include <iomanip>

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

class SocketGuard
{
public:
    explicit SocketGuard(socket_t sock = INVALID_SOCKET_VALUE) : socket_(sock) {}
    ~SocketGuard() { cleanup(); }
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

#ifdef _WIN32
class WSAInitGuard
{
public:
    WSAInitGuard() : initialized_(false) {}
    ~WSAInitGuard()
    {
        if (initialized_)
            WSACleanup();
    }
    bool init()
    {
        if (initialized_)
            return true;
        WSADATA wsaData;
        WSADATA wsaDataCorrect;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaDataCorrect);
        if (result == 0)
            initialized_ = true;
        return initialized_;
    }

private:
    bool initialized_;
};
#endif

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
            auth_key = std::get<std::string>(response.data_);
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Auth exception: " << e.what() << std::endl;
        return false;
    }
}

// Stats
struct BenchmarkStats
{
    int total_ops = 0;
    double duration_seconds = 0;
    double ops_per_sec = 0;
};

void run_benchmark(socket_t sock, const std::string &auth_key, const std::string &type_name, int count, std::function<std::string(int)> cmd_gen)
{
    std::cout << "Starting " << type_name << " Benchmark (" << count << " items)..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < count; ++i)
    {
        std::string cmd = cmd_gen(i);
        std::string req = serialize_request(RequestType::COMMAND, cmd, auth_key);
        if (!send_data(sock, req))
            break;
        try
        {
            Response resp = receive_server_response(sock);
            if (resp.code_ == 0)
                std::cerr << "Op " << i << " failed: " << resp.error_msg_ << std::endl;
        }
        catch (...)
        {
            std::cerr << "Exception at " << i << std::endl;
            break;
        }
        if (i > 0 && i % 10000 == 0)
            std::cout << "Processed " << i << "..." << std::endl;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    BenchmarkStats stats;
    stats.total_ops = count;
    stats.duration_seconds = diff.count();
    stats.ops_per_sec = count / stats.duration_seconds;

    std::cout << "Finished " << type_name << ": " << std::fixed << std::setprecision(2) << stats.ops_per_sec << " ops/sec (" << stats.duration_seconds << "s total)" << std::endl
              << std::endl;
}

void run_batch_benchmark(socket_t sock, const std::string &auth_key, const std::string &type_name, int count, int batch_size, std::function<std::string(int, int)> batch_cmd_gen)
{
    std::cout << "Starting " << type_name << " Batch Benchmark (" << count << " items, batch size " << batch_size << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    int batches = count / batch_size;
    for (int i = 0; i < batches; ++i)
    {
        std::string cmd = batch_cmd_gen(i * batch_size, batch_size);
        std::string req = serialize_request(RequestType::COMMAND, cmd, auth_key);
        if (!send_data(sock, req))
            break;
        try
        {
            Response resp = receive_server_response(sock);
            if (resp.code_ == 0)
                std::cerr << "Batch " << i << " failed: " << resp.error_msg_ << std::endl;
        }
        catch (...)
        {
            std::cerr << "Exception at batch " << i << std::endl;
            break;
        }
        if (i > 0 && i % (10000 / batch_size) == 0)
            std::cout << "Processed " << i * batch_size << "..." << std::endl;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    double ops_per_sec = count / diff.count();
    std::cout << "Finished " << type_name << " (Batch): " << std::fixed << std::setprecision(2) << ops_per_sec << " items/sec (" << diff.count() << "s total)" << std::endl
              << std::endl;
}

int main(int argc, char *argv[])
{
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    std::string password = "";
    int count = 50000;
    bool batch_mode = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            password = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--batch") == 0)
            batch_mode = true;
    }

#ifdef _WIN32
    WSAInitGuard wsa;
    if (!wsa.init())
        return 1;
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

    if (connect(socket_guard.get(), reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR_VALUE)
    {
        std::cerr << "Connect failed" << std::endl;
        return 1;
    }

    try
    {
        Response resp = receive_server_response(socket_guard.get());
        ConnectionState state = static_cast<ConnectionState>(stoi(std::get<std::string>(resp.data_)));
        if (state == ConnectionState::WAITING)
            receive_server_response(socket_guard.get());
    }
    catch (...)
    {
    }

    std::string auth_key = "";
    if (!authenticate(socket_guard, password, auth_key))
        return 1;

    std::cout << "Authenticated. Starting Stress Test with " << count << " items per type." << std::endl
              << std::endl;

    // String Test
    run_benchmark(socket_guard.get(), auth_key, "String SET", count, [](int i)
                  { return "set key1_" + std::to_string(i) + " value_" + std::to_string(i); });

    // List Test (Batching push 1 item effectively behaves like single, but if we want large list we can push 1 by 1)
    // To properly stress test list structure size, we push to the same LIST.
    if (batch_mode)
    {
        // Implement batch mode logic if needed, but for now let's stick to simple insertions or batched insertions
        // Batch Push to List
        run_batch_benchmark(socket_guard.get(), auth_key, "List RPUSH (Batch)", count, 100, [](int start, int size)
                            {
             std::string cmd = "rpush big_list";
             for(int i=0; i<size; ++i) cmd += " v_" + std::to_string(start + i);
             return cmd; });
    }
    else
    {
        run_benchmark(socket_guard.get(), auth_key, "List RPUSH", count, [](int i)
                      { return "rpush big_list v_" + std::to_string(i); });
    }

    // Set Test
    if (batch_mode)
    {
        run_batch_benchmark(socket_guard.get(), auth_key, "Set SADD (Batch)", count, 100, [](int start, int size)
                            {
             std::string cmd = "sadd big_set";
             for(int i=0; i<size; ++i) cmd += " m_" + std::to_string(start + i);
             return cmd; });
    }
    else
    {
        run_benchmark(socket_guard.get(), auth_key, "Set SADD", count, [](int i)
                      { return "sadd big_set m_" + std::to_string(i); });
    }

    // ZSet Test
    // Adding randomly distributed scores for skiplist testing
    if (batch_mode)
    {
        run_batch_benchmark(socket_guard.get(), auth_key, "ZSet ZADD (Batch)", count, 100, [](int start, int size)
                            {
             std::string cmd = "zadd big_zset";
             for(int i=0; i<size; ++i) {
                cmd += " " + std::to_string((start+i) * 1.5) + " m_" + std::to_string(start + i); // score member
             }
             return cmd; });
    }
    else
    {
        run_benchmark(socket_guard.get(), auth_key, "ZSet ZADD", count, [](int i)
                      {
            // member score
            return "zadd big_zset " + std::to_string(i * 1.5) + " m_" + std::to_string(i); });
    }

    // Hash Test
    if (batch_mode)
    {
        run_batch_benchmark(socket_guard.get(), auth_key, "Hash HSET (Batch)", count, 100, [](int start, int size)
                            {
             std::string cmd = "hset big_hash";
             for(int i=0; i<size; ++i) {
                cmd += " f_" + std::to_string(start+i) + " v_" + std::to_string(start + i); 
             }
             return cmd; });
    }
    else
    {
        run_benchmark(socket_guard.get(), auth_key, "Hash HSET", count, [](int i)
                      { return "hset big_hash f_" + std::to_string(i) + " v_" + std::to_string(i); });
    }

    std::cout << "Stress Test Complete." << std::endl;
    return 0;
}
