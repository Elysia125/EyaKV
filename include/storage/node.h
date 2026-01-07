#ifndef TINYKV_STORAGE_NODE_H_
#define TINYKV_STORAGE_NODE_H_

#include "common/types/value.h"
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
struct EValue
{
    EyaValue value;
    bool deleted;
    uint32_t expire_time = 0; // 0代表不会过期
    EValue() : value(), deleted(false) {}
    EValue(const EyaValue &value) : value(value), deleted(false) {}
    EValue(const EyaValue &value, bool deleted) : value(value), deleted(deleted) {}

    bool is_deleted() const
    {
        return deleted;
    }
    bool is_expired() const
    {
        return expire_time > 0 && expire_time < std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

inline std::string serialize(const EValue &value)
{
    std::string result;
    result.append(reinterpret_cast<const char *>(&value.deleted), sizeof(value.deleted));
    uint32_t expire = htonl(value.expire_time);
    result.append(reinterpret_cast<const char *>(&expire), sizeof(expire));
    result.append(serialize_eya_value(value.value));
    return result;
}

inline EValue deserialize(const char *data, size_t &offset)
{
    EValue result;
    std::memcpy(&result.deleted, data + offset, sizeof(result.deleted));
    offset += sizeof(result.deleted);
    uint32_t expire_time;
    std::memcpy(&expire_time, data + offset, sizeof(expire_time));
    offset += sizeof(expire_time);
    result.expire_time = ntohl(expire_time);
    result.value = deserialize_eya_value(data, offset);
    return result;
}

inline size_t estimateEValueSize(const EValue &value)
{
    return sizeof(EValue) + estimateEyaValueSize(value.value);
}

#endif // TINYKV_STORAGE_NODE_H_