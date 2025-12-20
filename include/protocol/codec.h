#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace tinykv::protocol {

enum class MessageType : uint8_t {
    kGet = 1,
    kPut = 2,
    kDelete = 3,
    kResponse = 4
};

struct Message {
    MessageType type;
    std::string key;
    std::string value;
    bool status = false; // For response
    std::string error;   // For response

    // Serialize to byte buffer
    std::vector<char> Encode() const {
        std::vector<char> buffer;
        
        // Type (1 byte)
        buffer.push_back(static_cast<char>(type));

        // Key Length (4 bytes) + Key
        uint32_t key_len = htonl(static_cast<uint32_t>(key.size()));
        const char* key_len_ptr = reinterpret_cast<const char*>(&key_len);
        buffer.insert(buffer.end(), key_len_ptr, key_len_ptr + 4);
        buffer.insert(buffer.end(), key.begin(), key.end());

        // Value Length (4 bytes) + Value
        uint32_t val_len = htonl(static_cast<uint32_t>(value.size()));
        const char* val_len_ptr = reinterpret_cast<const char*>(&val_len);
        buffer.insert(buffer.end(), val_len_ptr, val_len_ptr + 4);
        buffer.insert(buffer.end(), value.begin(), value.end());

        // Status (1 byte)
        buffer.push_back(status ? 1 : 0);

        // Error Length (4 bytes) + Error
        uint32_t err_len = htonl(static_cast<uint32_t>(error.size()));
        const char* err_len_ptr = reinterpret_cast<const char*>(&err_len);
        buffer.insert(buffer.end(), err_len_ptr, err_len_ptr + 4);
        buffer.insert(buffer.end(), error.begin(), error.end());

        return buffer;
    }

    // Deserialize from byte buffer
    static bool Decode(const std::vector<char>& buffer, Message& msg) {
        if (buffer.size() < 1) return false;
        size_t offset = 0;

        // Type
        msg.type = static_cast<MessageType>(buffer[offset]);
        offset += 1;

        // Key
        if (offset + 4 > buffer.size()) return false;
        uint32_t key_len;
        std::memcpy(&key_len, &buffer[offset], 4);
        key_len = ntohl(key_len);
        offset += 4;
        if (offset + key_len > buffer.size()) return false;
        msg.key.assign(&buffer[offset], key_len);
        offset += key_len;

        // Value
        if (offset + 4 > buffer.size()) return false;
        uint32_t val_len;
        std::memcpy(&val_len, &buffer[offset], 4);
        val_len = ntohl(val_len);
        offset += 4;
        if (offset + val_len > buffer.size()) return false;
        msg.value.assign(&buffer[offset], val_len);
        offset += val_len;

        // Status
        if (offset + 1 > buffer.size()) return false;
        msg.status = (buffer[offset] != 0);
        offset += 1;

        // Error
        if (offset + 4 > buffer.size()) return false;
        uint32_t err_len;
        std::memcpy(&err_len, &buffer[offset], 4);
        err_len = ntohl(err_len);
        offset += 4;
        if (offset + err_len > buffer.size()) return false;
        msg.error.assign(&buffer[offset], err_len);
        offset += err_len;

        return true;
    }
};

} // namespace tinykv::protocol
