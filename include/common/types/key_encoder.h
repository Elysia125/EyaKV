#ifndef COMMON_TYPES_KEY_ENCODER_H_
#define COMMON_TYPES_KEY_ENCODER_H_

#include <string>
#include <string_view>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include "common/util/encode_utils.h"
enum class ColumnFamily : uint8_t
{
    kDefault = 0,
    kHash = 1,
    kSet = 2,
    kZSetScore = 3,
    kZSetRank = 4,
    kList = 5
};

class KeyEncoder
{
public:
    inline static std::string FIXED_PREFIX = "__EYAKV__";
    /**
     * @brief 编码哈希表 (Hash) 域的子键
     * 格式: [CF(1字节)] [用户键长度(大端4字节)] [用户键] [版本号(大端8字节)] [段/域名称]
     */
    static std::string encode_hash_sub_key(std::string_view user_key, uint64_t version, std::string_view field)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8 + field.size());
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(ColumnFamily::kHash));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));
        result.append(field);
        return result;
    }

    /**
     * @brief 编码集合 (Set) 成员的子键
     * 格式: [CF(1字节)] [用户键长度(大端4字节)] [用户键] [版本号(大端8字节)] [成员名称]
     */
    static std::string encode_set_sub_key(std::string_view user_key, uint64_t version, std::string_view member)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8 + member.size());
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(ColumnFamily::kSet));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));
        result.append(member);
        return result;
    }

    /**
     * @brief 编码双端队列 (List/Deque) 的元素子键
     * 格式: [CF(1字节)] [用户键长度(大端4字节)] [用户键] [版本号(大端8字节)] [序列号(大端8字节)]
     */
    static std::string encode_list_sub_key(std::string_view user_key, uint64_t version, uint64_t sequence)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8 + 8);
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(ColumnFamily::kList));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));
        // 使用大端序以保证序列号在LSM-Tree中能被正确字典排序
        uint64_t seq_be = EncodeUtil::encode_u64_be(sequence);
        result.append(reinterpret_cast<const char *>(&seq_be), sizeof(seq_be));
        return result;
    }

    /**
     * @brief 编码有序集合 (ZSet) 的正向查找子键 (Member -> Score)
     * 格式: [CF(1字节)] [用户键长度(大端4字节)] [用户键] [版本号(大端8字节)] [成员名称]
     */
    static std::string encode_zset_lookup_key(std::string_view user_key, uint64_t version, std::string_view member)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8 + member.size());
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(ColumnFamily::kZSetScore));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));
        result.append(member);
        return result;
    }

    /**
     * @brief 编码有序集合 (ZSet) 的排序区间子键 (Score + Member -> 空实现排序)
     * 格式: [CF(1字节)] [用户键长度(大端4字节)] [用户键] [版本号(大端8字节)] [包装后分数(大端8字节)] [成员名称]
     * 注: 分数必须经过特殊包装与大端处理，来维持浮点数的自然递增排序
     */
    static std::string encode_zset_sort_key(std::string_view user_key, uint64_t version, double score, std::string_view member)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8 + 8 + member.size());
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(ColumnFamily::kZSetRank));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));

        uint64_t score_enc = encode_double_for_sort(score);
        result.append(reinterpret_cast<const char *>(&score_enc), sizeof(score_enc));
        result.append(member);
        return result;
    }

    // --- 前缀扫描辅助函数 (Prefix Scanning) ---

    /**
     * @brief 获取某复杂结构特定列族下所有子元素的前缀，用于范围查询 (Range Scan)
     */
    static std::string get_complex_prefix(ColumnFamily cf, std::string_view user_key, uint64_t version)
    {
        std::string result;
        result.reserve(FIXED_PREFIX.size() + 1 + 4 + user_key.size() + 8);
        result.append(FIXED_PREFIX);
        result.push_back(static_cast<char>(cf));
        uint32_t kl = htonl(static_cast<uint32_t>(user_key.size()));
        result.append(reinterpret_cast<const char *>(&kl), sizeof(kl));
        result.append(user_key);
        uint64_t ver_be = EncodeUtil::encode_u64_be(version);
        result.append(reinterpret_cast<const char *>(&ver_be), sizeof(ver_be));
        return result;
    }

    /**
     * @brief 获取前缀扫描的上界字符串 (用于基于 range 的 prefix 扫描，例如 range(prefix, get_prefix_end(prefix)))
     */
    static std::string get_prefix_end(const std::string &prefix)
    {
        std::string end = prefix;
        for (size_t i = end.size() - 1; i >= 0; --i)
        {
            if (static_cast<uint8_t>(end[i]) != 0xFF)
            {
                end[i]++;
                break;
            }
            end[i] = 0; // 若当前最高位溢出则本位清零并向前进位
        }
        return end;
    }

    // --- 解码辅助函数 (Decode Helpers) ---
    static std::string_view decode_hash_field(std::string_view sub_key)
    {
        return extract_suffix(sub_key);
    }

    static std::string_view decode_set_member(std::string_view sub_key)
    {
        return extract_suffix(sub_key);
    }

    static std::string_view decode_zset_lookup_member(std::string_view sub_key)
    {
        return extract_suffix(sub_key);
    }

    static std::pair<double, std::string> decode_zset_sort_key(std::string_view sub_key)
    {
        // 判断前缀合法性并提取用户键长度以定位成员名称的起始位置
        size_t prefix_len = FIXED_PREFIX.size() + 1 + 4; // 固定前缀 + 列族标识(1) + 键长(4)
        if (sub_key.size() < prefix_len)
            throw std::runtime_error("ivalid sort key: insufficient length for prefix and key length");

        uint32_t kl;
        std::memcpy(&kl, sub_key.data() + FIXED_PREFIX.size() + 1, sizeof(kl));
        kl = ntohl(kl);

        size_t offset = prefix_len + kl + 8; // 跳过用户键(kl)以及版本号(8)
        if (sub_key.size() < offset + 8)
            throw std::runtime_error("invalid sort key: insufficient length for score encoding");

        uint64_t enc_score;
        std::memcpy(&enc_score, sub_key.data() + offset, sizeof(enc_score));
        double score = decode_double_from_sort(enc_score);

        offset += 8;
        return {score, std::string(sub_key.data() + offset, sub_key.size() - offset)};
    }

    /**
     * @brief 安全编码64位浮点数用于根据 memcmp 进行排序
     * 对于 IEEE 754 标准，需要对负数域求反实现绝对排序统一
     * @param v 需编码排序的浮点数
     * @return 用于比对排序的大端无符号整型
     */
    static uint64_t encode_double_for_sort(double v)
    {
        uint64_t u;
        std::memcpy(&u, &v, sizeof(v));
        if (u & 0x8000000000000000ULL)
        { // 若为负数
            u = ~u;
        }
        else
        { // 若为正数
            u |= 0x8000000000000000ULL;
        }
        return EncodeUtil::encode_u64_be(u);
    }

    /**
     * @brief 还原上述安全的64位排序用浮点数
     */
    static double decode_double_from_sort(uint64_t v_be)
    {
        uint64_t u = EncodeUtil::decode_u64_be(v_be);
        if (u & 0x8000000000000000ULL)
        { // 曾是正数
            u &= ~0x8000000000000000ULL;
        }
        else
        { // 曾是负数
            u = ~u;
        }
        double v;
        std::memcpy(&v, &u, sizeof(u));
        return v;
    }

private:
    static std::string_view extract_suffix(std::string_view sub_key)
    {
        size_t prefix_len = FIXED_PREFIX.size() + 1 + 4; // 固定前缀 + 列族标识(1) + 键长(4)
        if (sub_key.size() < prefix_len)
        {
            throw std::runtime_error("invalid sub key: insufficient length for prefix and key length");
        }
        uint32_t kl;
        std::memcpy(&kl, sub_key.data() + FIXED_PREFIX.size() + 1, sizeof(kl));
        kl = ntohl(kl);
        size_t expected_prefix = FIXED_PREFIX.size() + 1 + 4 + kl + 8; // FIXED_PREFIX + CF + Len + Key + Version
        if (sub_key.size() <= expected_prefix)
        {
            throw std::runtime_error("invalid sub key: insufficient length for key and version");
        }
        return sub_key.substr(expected_prefix);
    }
};

#endif // COMMON_TYPES_KEY_ENCODER_H_
