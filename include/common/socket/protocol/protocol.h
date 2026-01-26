#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstring>
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

struct ProtocolHeader
{
    uint32_t magic_number = htonl(0xEA1314);
    uint8_t version = 1;
    uint32_t length = 0;
    static constexpr size_t PROTOCOL_HEADER_SIZE = 9;
    ProtocolHeader(uint32_t length = 0, uint8_t version = 1) : length(length), version(version) {}
    virtual std::string serialize() const
    {
        std::string result;
        uint32_t len = htonl(this->length);
        result.append(reinterpret_cast<const char *>(&magic_number), sizeof(magic_number));
        result.append(reinterpret_cast<const char *>(&version), sizeof(version));
        result.append(reinterpret_cast<const char *>(&len), sizeof(len));
        return result;
    }

    virtual void deserialize(const char *data, size_t &offset)
    {
        uint32_t magic_number;
        std::memcpy(&magic_number, data + offset, sizeof(magic_number));
        offset += sizeof(magic_number);
        if (ntohl(magic_number) != 0xEA1314)
        {
            throw std::runtime_error("Invalid magic number");
        }
        uint8_t version;
        std::memcpy(&version, data + offset, sizeof(version));
        offset += sizeof(version);
        uint32_t length;
        std::memcpy(&length, data + offset, sizeof(length));
        offset += sizeof(length);
        this->length = ntohl(length);
        this->version = version;
    }
};

struct ProtocolBody
{
    virtual std::string serialize() const = 0;
    virtual void deserialize(const char *data, size_t &offset) = 0;
    virtual ~ProtocolBody() = default;
};

#endif