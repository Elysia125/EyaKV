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
#include <atomic>

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

// 通用：读取响应并反序列化为 Response
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

// 专门给 batch / pipeline 用：按响应头中的 length 读取原始 body 字符串
std::string receive_raw_body(socket_t client_socket)
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

    std::string body;
    if (response_header.length > 0)
    {
        body.resize(response_header.length);
        int total_recv = 0;
        while (total_recv < static_cast<int>(response_header.length))
        {
            recv_len = recv(client_socket, &body[0] + total_recv,
                            static_cast<int>(response_header.length) - total_recv, 0);
            if (recv_len == SOCKET_ERROR_VALUE)
                throw std::runtime_error("recv body failed: " + socket_error_to_string(GET_SOCKET_ERROR()));
            total_recv += recv_len;
        }
    }
    return body;
}

bool send_data(socket_t client_socket, const std::string &data)
{
    ProtocolHeader header(data.size());
    std::string rdata = header.serialize() + data;
    int total_sent = 0;
    int remaining = static_cast<int>(rdata.size());

    while (remaining > 0)
    {
        int sent = send(client_socket, rdata.c_str() + total_sent, remaining, 0);
        if (sent == 0)
        {
            std::cout << "server closed connection" << std::endl;
            exit(0);
        }
        if (sent == SOCKET_ERROR_VALUE)
        {
            return false;
        }
        total_sent += sent;
        remaining -= sent;
    }

    return true;
}

bool authenticate(SocketGuard &socket_guard, const std::string &password, std::string &auth_key)
{
    Request req=Request::auth(generate_random_string(16),password);
    std::string data = req.serialize();
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

// 建立到 EyaKV 的连接并完成握手和认证
bool setup_connection(const std::string &host, int port, const std::string &password,
                      SocketGuard &socket_guard, std::string &auth_key)
{
    socket_guard.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket_guard.is_valid())
    {
        std::cerr << "Socket create failed" << std::endl;
        return false;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid address" << std::endl;
        return false;
    }

    if (connect(socket_guard.get(), reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR_VALUE)
    {
        std::cerr << "Connect failed" << std::endl;
        return false;
    }

    try
    {
        Response resp = receive_server_response(socket_guard.get());
        ConnectionState state = static_cast<ConnectionState>(stoi(std::get<std::string>(resp.data_)));
        if (state == ConnectionState::WAITING)
        {
            receive_server_response(socket_guard.get());
        }
    }
    catch (...)
    {
        // 忽略握手阶段异常，由后续认证结果决定是否可用
    }

    if (!authenticate(socket_guard, password, auth_key))
    {
        return false;
    }

    return true;
}

// Stats
struct BenchmarkStats
{
    int total_ops = 0;
    double duration_seconds = 0;
    double ops_per_sec = 0;
};

// 多线程压测结果
struct ThreadBenchmarkResult
{
    int thread_id = 0;
    int total_ops = 0;
    double duration_seconds = 0.0;
    double ops_per_sec = 0.0;
};

void run_benchmark(socket_t sock, const std::string &auth_key, const std::string &type_name, int count, std::function<std::string(int)> cmd_gen)
{
    std::cout << "Starting " << type_name << " Benchmark (" << count << " items)..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < count; ++i)
    {
        std::string cmd = cmd_gen(i);
        Request request = Request::createCommand(generate_random_string(16), cmd, auth_key);
        if (!send_data(sock, request.serialize()))
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
        Request request = Request::createCommand(generate_random_string(16), cmd, auth_key);
        if (!send_data(sock, request.serialize()))
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

// 测试服务端能同时建立的最大连接数（逐个建立并保持连接）
void run_connection_limit_test(const std::string &host, int port, const std::string &password, int max_connections)
{
    std::cout << "Starting connection limit test (target: " << max_connections << " connections)..." << std::endl;
    std::vector<std::unique_ptr<SocketGuard>> connections;
    connections.reserve(max_connections);

    int success = 0;
    for (int i = 0; i < max_connections; ++i)
    {
        auto guard = std::make_unique<SocketGuard>();
        std::string auth_key;
        if (!setup_connection(host, port, password, *guard, auth_key))
        {
            std::cerr << "Connection " << i << " failed, stop." << std::endl;
            break;
        }
        connections.emplace_back(std::move(guard));
        ++success;
    }

    std::cout << "Connection limit test finished: established " << success << " / " << max_connections << " connections." << std::endl
              << std::endl;
}

// 多连接（多线程）下的字符串写入吞吐量压测
// use_pipeline 为 true 时使用 BATCH_COMMAND，否则使用单条 COMMAND
void run_multi_thread_throughput(const std::string &host, int port, const std::string &password,
                                 int threads, int count_per_thread,
                                 bool use_pipeline, int pipeline_batch_size)
{
    std::cout << "Starting multi-thread throughput test: " << threads
              << " threads, " << count_per_thread << " ops per thread (String SET)"
              << (use_pipeline ? ", pipeline batch " + std::to_string(pipeline_batch_size) : "")
              << "..." << std::endl;

    std::vector<ThreadBenchmarkResult> results(threads);

    auto worker = [&](int tid)
    {
        SocketGuard socket_guard;
        std::string auth_key;
        if (!setup_connection(host, port, password, socket_guard, auth_key))
        {
            std::cerr << "Thread " << tid << ": connection/auth failed, aborting thread." << std::endl;
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        int done = 0;

        if (use_pipeline)
        {
            int index = 0;
            while (index < count_per_thread)
            {
                std::vector<std::pair<std::string, std::string>> cmds;
                cmds.reserve(pipeline_batch_size);
                for (int j = 0; j < pipeline_batch_size && index < count_per_thread; ++j, ++index)
                {
                    std::string cmd = "set mt_key_" + std::to_string(tid) + "_" + std::to_string(index) +
                                      " mt_value_" + std::to_string(index);
                    cmds.emplace_back("cmd_" + std::to_string(index), cmd);
                }
                Request req = Request::createBatchCommand(generate_random_string(16), cmds, auth_key);
                if (!send_data(socket_guard.get(), req.serialize()))
                {
                    std::cerr << "Thread " << tid << ": pipeline send failed at index " << index << std::endl;
                    break;
                }
                try
                {
                    std::string raw_body = receive_raw_body(socket_guard.get());
                    size_t offset = 0;
                    auto batch_res = deserializeBatchResponse(raw_body.data(), offset);
                    for (const auto &kv : batch_res.second)
                    {
                        if (!kv.second.is_success())
                        {
                            std::cerr << "Thread " << tid << ": sub " << kv.first << " failed: "
                                      << kv.second.error_msg_ << std::endl;
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Thread " << tid << ": pipeline recv exception: " << e.what() << std::endl;
                    break;
                }
                done = index;
            }
        }
        else
        {
            for (int i = 0; i < count_per_thread; ++i)
            {
                std::string cmd = "set mt_key_" + std::to_string(tid) + "_" + std::to_string(i) +
                                  " mt_value_" + std::to_string(i);
                Request req = Request::createCommand(generate_random_string(16), cmd, auth_key);

                if (!send_data(socket_guard.get(), req.serialize()))
                {
                    std::cerr << "Thread " << tid << ": send failed at op " << i << std::endl;
                    break;
                }
                try
                {
                    Response resp = receive_server_response(socket_guard.get());
                    if (resp.code_ == 0)
                    {
                        std::cerr << "Thread " << tid << ": op " << i << " failed: " << resp.error_msg_ << std::endl;
                    }
                }
                catch (...)
                {
                    std::cerr << "Thread " << tid << ": exception at op " << i << std::endl;
                    break;
                }
                ++done;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;

        results[tid].thread_id = tid;
        results[tid].total_ops = done;
        results[tid].duration_seconds = diff.count();
        results[tid].ops_per_sec = diff.count() > 0.0 ? done / diff.count() : 0.0;
    };

    auto global_start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> ths;
    ths.reserve(threads);
    for (int t = 0; t < threads; ++t)
    {
        ths.emplace_back(worker, t);
    }
    for (auto &th : ths)
    {
        if (th.joinable())
            th.join();
    }
    auto global_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> global_diff = global_end - global_start;

    long long total_ops = 0;
    for (const auto &r : results)
    {
        total_ops += r.total_ops;
    }
    double global_qps = global_diff.count() > 0.0 ? total_ops / global_diff.count() : 0.0;

    std::cout << "Multi-thread throughput summary:" << std::endl;
    for (const auto &r : results)
    {
        std::cout << "  Thread " << r.thread_id << ": " << r.total_ops << " ops, "
                  << std::fixed << std::setprecision(2) << r.ops_per_sec << " ops/sec" << std::endl;
    }
    std::cout << "  Total: " << total_ops << " ops, "
              << std::fixed << std::setprecision(2) << global_qps << " ops/sec ("
              << global_diff.count() << "s total)" << std::endl
              << std::endl;
}

// 使用 BATCH_COMMAND + pipeline 的压测
void run_pipeline_benchmark(socket_t sock, const std::string &auth_key,
                            const std::string &type_name, int count, int batch_size,
                            std::function<std::string(int)> cmd_gen)
{
    std::cout << "Starting " << type_name << " Pipeline Benchmark (" << count
              << " items, pipeline batch size " << batch_size << ")..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    int sent_ops = 0;
    int index = 0;
    while (index < count)
    {
        std::vector<std::pair<std::string, std::string>> cmds;
        cmds.reserve(batch_size);
        for (int j = 0; j < batch_size && index < count; ++j, ++index)
        {
            std::string cmd = cmd_gen(index);
            cmds.emplace_back("cmd_" + std::to_string(index), cmd);
        }

        Request req = Request::createBatchCommand(generate_random_string(16), cmds, auth_key);
        if (!send_data(sock, req.serialize()))
        {
            std::cerr << "Pipeline send failed." << std::endl;
            break;
        }

        try
        {
            // 按响应头长度读取 body 字符串，并用 deserializeBatchResponse 反序列化
            std::string raw_body = receive_raw_body(sock);
            size_t offset = 0;
            auto batch_vec = deserializeBatchResponse(raw_body.data(), offset);
            // 遍历子响应，检查是否有错误
            for (const auto &kv : batch_vec.second)
            {
                if (!kv.second.is_success())
                {
                    std::cerr << "Sub command " << kv.first << " failed: "
                              << kv.second.error_msg_ << std::endl;
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Exception in pipeline batch: " << e.what() << std::endl;
            break;
        }

        sent_ops = index;
        if (sent_ops > 0 && sent_ops % 10000 == 0)
            std::cout << "Pipeline processed " << sent_ops << "..." << std::endl;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    double ops_per_sec = sent_ops > 0 && diff.count() > 0.0 ? sent_ops / diff.count() : 0.0;

    std::cout << "Finished " << type_name << " (Pipeline): " << std::fixed
              << std::setprecision(2) << ops_per_sec << " items/sec (" << diff.count()
              << "s total)" << std::endl
              << std::endl;
}

int main(int argc, char *argv[])
{
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    std::string password = "";
    int count = 50000;
    bool batch_mode = false;
    int multi_threads = 0;          // 多线程压测时的线程数
    int conn_limit_max = 0;         // 连接数上限测试的最大尝试连接数
    bool skip_single_test = false;  // 跳过单连接多数据结构测试
    bool skip_conn_limit = false;   // 跳过连接数上限测试
    bool skip_multi_thread = false; // 跳过多线程吞吐量测试
    bool pipeline_mode = false;     // 是否启用 pipeline
    int pipeline_batch_size = 50;   // pipeline 批次大小

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
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            multi_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--conn-limit") == 0 && i + 1 < argc)
            conn_limit_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--skip-single") == 0)
            skip_single_test = true;
        else if (strcmp(argv[i], "--skip-conn-limit") == 0)
            skip_conn_limit = true;
        else if (strcmp(argv[i], "--skip-multi-thread") == 0)
            skip_multi_thread = true;
        else if (strcmp(argv[i], "--pipeline") == 0)
            pipeline_mode = true;
        else if (strcmp(argv[i], "--pipeline-batch") == 0 && i + 1 < argc)
        {
            pipeline_batch_size = atoi(argv[++i]);
            if (pipeline_batch_size <= 0)
            {
                std::cerr << "Invalid --pipeline-batch value, use default 50" << std::endl;
                pipeline_batch_size = 50;
            }
        }
        else
        {
            std::cerr << "Unknown argument: " << argv[i] << std::endl;
        }
    }

#ifdef _WIN32
    WSAInitGuard wsa;
    if (!wsa.init())
        return 1;
#endif

    SocketGuard socket_guard;
    std::string auth_key = "";
    if (!setup_connection(host, port, password, socket_guard, auth_key))
        return 1;

    std::cout << "Authenticated. Starting Stress Test with " << count
              << " items per type." << std::endl;
    if (pipeline_mode)
    {
        std::cout << "Pipeline mode: ON, batch size = " << pipeline_batch_size
                  << " (using BATCH_COMMAND)" << std::endl;
    }
    else
    {
        std::cout << "Pipeline mode: OFF (using single COMMAND per request)" << std::endl;
    }
    std::cout << std::endl;

    // 单连接多数据结构测试
    if (!skip_single_test)
    {
        // String Test
        if (pipeline_mode)
        {
            run_pipeline_benchmark(socket_guard.get(), auth_key, "String SET (Pipeline)", count,
                                   pipeline_batch_size,
                                   [](int i)
                                   { return "set key1_" + std::to_string(i) + " value_" + std::to_string(i); });
        }
        else
        {
            run_benchmark(socket_guard.get(), auth_key, "String SET", count, [](int i)
                          { return "set key1_" + std::to_string(i) + " value_" + std::to_string(i); });
        }

        // List Test
        if (pipeline_mode)
        {
            run_pipeline_benchmark(socket_guard.get(), auth_key, "List RPUSH (Pipeline)", count,
                                   pipeline_batch_size,
                                   [](int i)
                                   { return "rpush big_list v_" + std::to_string(i); });
        }
        else if (batch_mode)
        {
            run_batch_benchmark(socket_guard.get(), auth_key, "List RPUSH (Batch)", count, 100,
                                [](int start, int size)
                                {
                                    std::string cmd = "rpush big_list";
                                    for (int i = 0; i < size; ++i)
                                        cmd += " v_" + std::to_string(start + i);
                                    return cmd;
                                });
        }
        else
        {
            run_benchmark(socket_guard.get(), auth_key, "List RPUSH", count, [](int i)
                          { return "rpush big_list v_" + std::to_string(i); });
        }

        // Set Test
        if (pipeline_mode)
        {
            run_pipeline_benchmark(socket_guard.get(), auth_key, "Set SADD (Pipeline)", count,
                                   pipeline_batch_size,
                                   [](int i)
                                   { return "sadd big_set m_" + std::to_string(i); });
        }
        else if (batch_mode)
        {
            run_batch_benchmark(socket_guard.get(), auth_key, "Set SADD (Batch)", count, 100,
                                [](int start, int size)
                                {
                                    std::string cmd = "sadd big_set";
                                    for (int i = 0; i < size; ++i)
                                        cmd += " m_" + std::to_string(start + i);
                                    return cmd;
                                });
        }
        else
        {
            run_benchmark(socket_guard.get(), auth_key, "Set SADD", count, [](int i)
                          { return "sadd big_set m_" + std::to_string(i); });
        }

        // ZSet Test
        if (pipeline_mode)
        {
            run_pipeline_benchmark(socket_guard.get(), auth_key, "ZSet ZADD (Pipeline)", count,
                                   pipeline_batch_size,
                                   [](int i)
                                   {
                                       return "zadd big_zset " + std::to_string(i * 1.5) +
                                              " m_" + std::to_string(i);
                                   });
        }
        else if (batch_mode)
        {
            run_batch_benchmark(socket_guard.get(), auth_key, "ZSet ZADD (Batch)", count, 100,
                                [](int start, int size)
                                {
                                    std::string cmd = "zadd big_zset";
                                    for (int i = 0; i < size; ++i)
                                    {
                                        cmd += " " + std::to_string((start + i) * 1.5) +
                                               " m_" + std::to_string(start + i); // score member
                                    }
                                    return cmd;
                                });
        }
        else
        {
            run_benchmark(socket_guard.get(), auth_key, "ZSet ZADD", count, [](int i)
                          {
                              // member score
                              return "zadd big_zset " + std::to_string(i * 1.5) + " m_" + std::to_string(i);
                          });
        }

        // Hash Test
        if (pipeline_mode)
        {
            run_pipeline_benchmark(socket_guard.get(), auth_key, "Hash HSET (Pipeline)", count,
                                   pipeline_batch_size,
                                   [](int i)
                                   {
                                       return "hset big_hash f_" + std::to_string(i) + " v_" +
                                              std::to_string(i);
                                   });
        }
        else if (batch_mode)
        {
            run_batch_benchmark(socket_guard.get(), auth_key, "Hash HSET (Batch)", count, 100,
                                [](int start, int size)
                                {
                                    std::string cmd = "hset big_hash";
                                    for (int i = 0; i < size; ++i)
                                    {
                                        cmd += " f_" + std::to_string(start + i) +
                                               " v_" + std::to_string(start + i);
                                    }
                                    return cmd;
                                });
        }
        else
        {
            run_benchmark(socket_guard.get(), auth_key, "Hash HSET", count, [](int i)
                          { return "hset big_hash f_" + std::to_string(i) + " v_" + std::to_string(i); });
        }
    }

    // 连接数上限测试（可选）
    if (!skip_conn_limit && conn_limit_max > 0)
    {
        run_connection_limit_test(host, port, password, conn_limit_max);
    }

    // 多连接（多线程）吞吐量测试（可选）
    if (!skip_multi_thread && multi_threads > 0)
    {
        run_multi_thread_throughput(host, port, password, multi_threads, count,
                                    pipeline_mode, pipeline_batch_size);
    }

    std::cout << "Stress Test Complete." << std::endl;
    return 0;
}
