#ifndef SOCKET_H
#define SOCKET_H
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iostream>
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
// 统一平台的一些变量
#ifdef _WIN32
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define SOCKET_ERROR_VALUE SOCKET_ERROR
#define CLOSE_SOCKET closesocket
#define GET_SOCKET_ERROR WSAGetLastError
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
inline std::string socket_error_to_string(int error) { return std::to_string(error); }
#else
typedef int socket_t;
#define INVALID_SOCKET_VALUE -1
#define SOCKET_ERROR_VALUE -1
#define CLOSE_SOCKET close
#define GET_SOCKET_ERROR() errno
inline std::string socket_error_to_string(int error) { return strerror(error); }
#endif

struct Address
{
    std::string host;
    int port;
    Address(const std::string &host, int port) : host(host), port(port) {}

    Address() : host(""), port(0) {}

    bool operator==(const Address &other) const
    {
        return host == other.host && port == other.port;
    }

    bool is_null() const
    {
        return host.empty() && port == 0;
    }

    std::string to_string() const
    {
        return host + ":" + std::to_string(port);
    }

    std::string serialize() const
    {
        std::string data;
        uint32_t host_len = host.size();
        host_len = htonl(host_len);
        data.append(reinterpret_cast<const char *>(&host_len), sizeof(host_len));
        data.append(host);
        uint32_t port_net = htonl(port);
        data.append(reinterpret_cast<const char *>(&port_net), sizeof(port_net));
        return data;
    }

    static Address deserialize(const char *data, size_t &offset)
    {
        uint32_t host_len;
        std::memcpy(&host_len, data + offset, sizeof(host_len));
        offset += sizeof(host_len);
        host_len = ntohl(host_len);
        std::string host(data + offset, host_len);
        offset += host_len;
        uint32_t port_net;
        std::memcpy(&port_net, data + offset, sizeof(port_net));
        offset += sizeof(port_net);
        port_net = ntohl(port_net);
        return Address(host, port_net);
    }
};

namespace std
{
    template <>
    struct hash<Address>
    {
        std::size_t operator()(const Address &addr) const
        {
            std::string data = addr.to_string();
            return std::hash<std::string>()(data);
        }
    };
}

/**
 * @brief 对socket的RAII封装
 */
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

#ifdef _WIN32
/**
 * @brief 对WSAStartup/WSACleanup的RAII封装
 */
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
/**
 * @brief 跨平台发送数据（保证完整发送指定数据，处理信号中断和无效socket）
 * @param client_socket 客户端socket句柄（跨平台类型）
 * @param data 要发送的字符串数据
 * @return 成功返回实际发送的字节数（等于data.size()）；失败返回SOCKET_ERROR_VALUE（可通过GET_SOCKET_ERROR查原因）
 * @note 循环发送直到全部发送完成或出错，处理Linux下EINTR信号中断重试
 */
inline int send_data(socket_t client_socket, const std::string &data)
{
    // 前置检查：无效socket
    if (client_socket == INVALID_SOCKET_VALUE)
    {
        return SOCKET_ERROR_VALUE;
    }

    // 空数据直接返回0（无数据可发）
    uint32_t data_len = static_cast<uint32_t>(data.size());
    if (data_len == 0)
    {
        return 0;
    }

    uint32_t total_sent = 0;
    uint32_t remaining = data_len;

    while (remaining > 0)
    {
        // 跨平台send参数适配：Windows需reinterpret_cast<char*>，Linux直接用const char*
        int sent = 0;
#ifdef _WIN32
        sent = send(client_socket, data.c_str() + total_sent,
                    static_cast<int>(remaining), 0);
#else
        // Linux下处理EINTR信号中断：重试send
        do
        {
            sent = send(client_socket, data.c_str() + total_sent, remaining, 0);
        } while (sent == SOCKET_ERROR_VALUE && GET_SOCKET_ERROR() == EINTR);
#endif

        // 处理发送错误
        if (sent <= 0)
        {
            return sent;
        }

        total_sent += sent;
        remaining -= sent;
    }

    return total_sent;
}

/**
 * @brief 跨平台接收数据（无副作用，区分超时/连接关闭/其他错误）
 * @param client_socket 客户端socket句柄（跨平台类型）
 * @param data 输出参数，存储接收的数据
 * @param data_size 期望接收的字节数
 * @param timeout 超时时间（单位：毫秒），0表示无超时
 * @return 成功返回实际接收的字节数；失败返回：
 *         -1：超时；-2：对方正常关闭连接；-3：其他错误（可通过GET_SOCKET_ERROR查原因）
 * @note 1. 保存/恢复原有超时配置，不影响其他recv调用；
 *       2. 跨平台适配超时设置（Windows用DWORD，Linux用timeval）；
 *       3. Linux下处理EINTR信号中断重试
 */
inline int receive_data(socket_t client_socket, std::string &data, uint32_t data_size, uint32_t timeout = 0)
{
    // 前置检查：无效socket或无效数据长度
    if (client_socket == INVALID_SOCKET_VALUE || data_size == 0)
    {
        data.clear();
        return -3;
    }

    // 初始化缓冲区：先清空，预分配空间提升性能
    data.clear();
    data.reserve(data_size);
    data.resize(data_size);

    //  步骤1：保存socket原有超时配置（避免副作用）
    bool has_old_timeout = false;
#ifdef _WIN32
    DWORD old_timeout = 0;
    int old_timeout_len = sizeof(old_timeout);
    // Windows：获取原有SO_RCVTIMEO配置（DWORD，毫秒）
    if (getsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&old_timeout, &old_timeout_len) != SOCKET_ERROR_VALUE)
    {
        has_old_timeout = true;
    }
    // 设置本次临时超时（Windows直接用毫秒）
    DWORD new_timeout = (timeout > 0) ? timeout : 0;
#else
    struct timeval old_tv;
    socklen_t old_tv_len = sizeof(old_tv);
    // Linux：获取原有SO_RCVTIMEO配置（timeval）
    if (getsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &old_tv, &old_tv_len) != SOCKET_ERROR_VALUE)
    {
        has_old_timeout = true;
    }
    // 设置本次临时超时（拆分毫秒到秒+微秒）
    struct timeval new_tv;
    if (timeout > 0)
    {
        new_tv.tv_sec = timeout / 1000;
        new_tv.tv_usec = (timeout % 1000) * 1000;
    }
    else
    {
        new_tv.tv_sec = 0;
        new_tv.tv_usec = 0; // 无超时
    }
#endif

    //  步骤2：设置临时超时配置
    bool set_opt_ok = false;
#ifdef _WIN32
    set_opt_ok = (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&new_timeout, sizeof(new_timeout)) != SOCKET_ERROR_VALUE);
#else
    set_opt_ok = (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &new_tv, sizeof(new_tv)) != SOCKET_ERROR_VALUE);
#endif
    if (!set_opt_ok)
    {
        data.clear();
        // 即使设置失败，也要尝试恢复原有配置
        if (has_old_timeout)
        {
            if (has_old_timeout)
            {
#ifdef _WIN32
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&old_timeout, sizeof(old_timeout));
#else
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
#endif
            }
        }
        return -3;
    }

    //  步骤3：循环接收直到达到指定长度或发生错误
    uint32_t total_received = 0;
    while (total_received < data_size)
    {
        uint32_t remaining = data_size - total_received;
        int received = 0;

#ifdef _WIN32
        // Windows无EINTR，直接调用recv
        received = recv(client_socket, &data[total_received], static_cast<int>(remaining), 0);
#else
        // Linux处理EINTR信号中断：重试recv
        do
        {
            received = recv(client_socket, &data[total_received], remaining, 0);
        } while (received == SOCKET_ERROR_VALUE && GET_SOCKET_ERROR() == EINTR);
#endif

        // 处理recv返回值
        if (received > 0)
        {
            total_received += received;
        }
        else if (received == 0)
        {
            // 对方正常关闭连接
            if (has_old_timeout)
            {
#ifdef _WIN32
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&old_timeout, sizeof(old_timeout));
#else
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
#endif
            }
            data.clear();
            return -2;
        }
        else // received == SOCKET_ERROR_VALUE
        {
            // 区分超时和其他错误
            int err_code = GET_SOCKET_ERROR();
            bool is_timeout = false;
#ifdef _WIN32
            if (err_code == WSAETIMEDOUT)
            {
                is_timeout = true;
            }
#else
            if (err_code == EAGAIN || err_code == EWOULDBLOCK)
            {
                is_timeout = true;
            }
#endif

            // 恢复原有超时配置
            if (has_old_timeout)
            {
#ifdef _WIN32
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&old_timeout, sizeof(old_timeout));
#else
                setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
#endif
            }

            if (is_timeout)
            {
                data.clear();
                return -1; // 超时
            }
            else
            {
                // 其他错误（如连接重置、无效socket等）
                data.clear();
                return -3;
            }
        }
    }

    // 步骤4：恢复原有超时配置（成功接收完整数据）
    if (has_old_timeout)
    {
#ifdef _WIN32
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&old_timeout, sizeof(old_timeout));
#else
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
#endif
    }

    // 步骤5：成功返回
    return static_cast<int>(total_received);
}

/**
 * @brief 连接信息结构体
 *
 * 用于存储 TCP 连接的相关信息，包括 socket 描述符、
 * 连接时间和客户端地址等。主要用于等待队列和未认证连接集合。
 */
struct Connection
{
    socket_t socket;                                  // socket描述符
    std::chrono::steady_clock::time_point start_time; // 等待开始时间/就绪开始时间
    sockaddr_in client_addr;                          // 客户端地址信息

    /**
     * @brief 相等性比较运算符
     *
     * @param other 另一个 Connection 对象
     * @return true 如果 socket 描述符相同
     */
    bool operator==(const Connection &other) const
    {
        return socket == other.socket;
    }
};

namespace std
{
    template <>
    struct hash<Connection>
    {
        size_t operator()(const Connection &conn) const
        {
            return std::hash<socket_t>()(conn.socket);
        }
    };
}

/**
 * @brief 检测主机字节序（大端/小端）
 * @return true：大端序；false：小端序
 */
inline bool is_big_endian()
{
    union
    {
        uint32_t val;
        uint8_t bytes[4];
    } test = {0x01020304}; // 大端序存储为 01 02 03 04，小端为 04 03 02 01
    return (test.bytes[0] == 0x01);
}

using get_address_func = std::function<int(socket_t s, sockaddr *name, int *namelen)>;

inline bool get_address(get_address_func func, socket_t sock, Address &result)
{
    sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (func(sock, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0)
    {
        std::cerr << "获取地址信息失败" << std::endl;
        return false;
    }
    char ip_buf[INET6_ADDRSTRLEN];
    uint16_t port = 0;
    size_t ip_buf_len = sizeof(ip_buf);
    switch (addr.ss_family)
    {
    case AF_INET:
    { // IPv4
        const struct sockaddr_in *ipv4_addr = reinterpret_cast<const struct sockaddr_in *>(&addr);
        // 转换IP：二进制→字符串（网络字节序→可读）
        if (inet_ntop(AF_INET, &ipv4_addr->sin_addr, ip_buf, ip_buf_len) == nullptr)
        {
            std::cerr << "IPv4地址转换失败" << std::endl;
            return false;
        }
        // 转换端口：网络字节序→主机字节序
        port = ntohs(ipv4_addr->sin_port);
        break;
    }
    case AF_INET6:
    { // IPv6
        const struct sockaddr_in6 *ipv6_addr = reinterpret_cast<const struct sockaddr_in6 *>(&addr);
        // 转换IP：IPv6二进制→字符串（如2001:0db8::1）
        if (inet_ntop(AF_INET6, &ipv6_addr->sin6_addr, ip_buf, ip_buf_len) == nullptr)
        {
            std::cerr << "IPv6地址转换失败" << std::endl;
            return false;
        }
        // 转换端口：IPv6端口同样用ntohs转换
        port = ntohs(ipv6_addr->sin6_port);
        break;
    }
    default:
    {
        std::cerr << "不支持的地址族：" << addr.ss_family << std::endl;
        return false;
    }
    }
    result.host = std::string(ip_buf);
    result.port = port;
    return true;
}

/**
 * @brief 根据socket获取本地地址信息
 */
inline bool get_self_address(socket_t sock, Address &result)
{
    return get_address(getsockname, sock, result);
}

/**
 *  @brief 根据socket获取对方地址信息
 */
inline bool get_opposite_address(socket_t sock, Address &result)
{
    return get_address(getpeername, sock, result);
}

#endif