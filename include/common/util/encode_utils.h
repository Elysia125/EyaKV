#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

// 字节序与编码工具
class EncodeUtil
{
public:
    /**
     * @brief 进行 64 位无符号整数的主机字节序到网络字节序（大端序）的转换
     */
    static uint64_t encode_u64_be(uint64_t v)
    {
#ifdef _WIN32
        return _byteswap_uint64(v);
#else
        return __builtin_bswap64(v);
#endif
    }

    static uint64_t decode_u64_be(uint64_t v)
    {
#ifdef _WIN32
        return _byteswap_uint64(v);
#else
        return __builtin_bswap64(v);
#endif
    }

    // 将 double 转为可按字节比较的 uint64_t
    static uint64_t encode_double(double val)
    {
        uint64_t u;
        std::memcpy(&u, &val, sizeof(val));
        // IEEE 754 浮点数规则：
        // 如果是正数，将符号位(最高位)翻转为1；如果是负数，将所有位翻转。
        if ((u & 0x8000000000000000ULL) == 0)
        {
            u |= 0x8000000000000000ULL;
        }
        else
        {
            u = ~u;
        }
        return u;
    }

    // 解码 double
    static double decode_double(uint64_t u)
    {
        if ((u & 0x8000000000000000ULL) != 0)
        {
            u &= ~0x8000000000000000ULL;
        }
        else
        {
            u = ~u;
        }
        double val;
        std::memcpy(&val, &u, sizeof(u));
        return val;
    }
};