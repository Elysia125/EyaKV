#ifndef TINYKV_INCLUDE_COMMON_SERIALIZE_H_
#define TINYKV_INCLUDE_COMMON_SERIALIZE_H_
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include "common/ds/skip_list.h"
#include "common/ds/zset.h"
namespace EyaKV
{

    /**
     * @brief 序列化工具类
     *
     * 提供对 string、vector<string>、unordered_set<string>、
     * unordered_map<string,string> 和 ZSet 的序列化和反序列化功能。
     *
     * 数据格式：
     * - string: [4字节长度][数据]
     * - vector<string>: [4字节元素个数][string1][string2]...
     * - unordered_set<string>: [4字节元素个数][string1][string2]...
     * - unordered_map<string,string>: [4字节元素个数][key1][value1][key2][value2]...
     */
    class Serializer
    {
    public:
        // 序列化到字节流

        /**
         * @brief 序列化 string 到字节流
         */
        static std::string serialize(const std::string &str)
        {
            std::string result;
            uint32_t len = htonl(static_cast<uint32_t>(str.size()));
            result.append(reinterpret_cast<const char *>(&len), sizeof(len));
            result.append(str);
            return result;
        }

        /**
         * @brief 序列化 vector<string> 到字节流
         */
        static std::string serialize(const std::vector<std::string> &vec)
        {
            std::string result;
            uint32_t size = htonl(static_cast<uint32_t>(vec.size()));
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &str : vec)
            {
                result.append(serialize(str));
            }
            return result;
        }

        /**
         * @brief 序列化 vector<pair<string, string>> 到字节流
         */
        static std::string serialize(const std::vector<std::pair<std::string, std::string>> &vec)
        {
            std::string result;
            uint32_t size = htonl(static_cast<uint32_t>(vec.size()));
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &[key, value] : vec)
            {
                result.append(serialize(key));
                result.append(serialize(value));
            }
            return result;
        }

        /**
         * @brief 序列化 deque<string> 到字节流
         */
        static std::string serialize(const std::deque<std::string> &deque)
        {
            std::string result;
            uint32_t size = htonl(static_cast<uint32_t>(deque.size()));
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &str : deque)
            {
                result.append(serialize(str));
            }
            return result;
        }
        /**
         * @brief 序列化 unordered_set<string> 到字节流
         */
        static std::string serialize(const std::unordered_set<std::string> &set)
        {
            std::string result;
            uint32_t size = htonl(static_cast<uint32_t>(set.size()));
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &str : set)
            {
                result.append(serialize(str));
            }
            return result;
        }
        static std::string serialize(double v)
        {
            // 直接存 8 字节二进制，最高效
            return std::string(reinterpret_cast<const char *>(&v), sizeof(double));
        }
        // ZSet 专用复合类型 ---

        static std::string serialize(const ZSetKey &key)
        {
            std::string res = serialize(key.score);
            res.append(serialize(key.member));
            return res;
        }

        /**
         * @brief 序列化 SkipList<ZSetKey, std::string>
         */
        static std::string serialize(const SkipList<ZSetKey, std::string> &sl)
        {
            // 适配 SkipList 的成员函数指针要求
            auto sk_func = [](const ZSetKey &k)
            { return Serializer::serialize(k); };
            auto sv_func = [](const std::string &v)
            { return Serializer::serialize(v); };
            return sl.serialize(sk_func, sv_func);
        }

        /**
         * @brief 序列化 unordered_map<string, double>
         */
        static std::string serialize(const std::unordered_map<std::string, double> &map)
        {
            uint32_t size = htonl(static_cast<uint32_t>(map.size()));
            std::string res;
            res.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (auto &p : map)
            {
                res.append(serialize(p.first));
                res.append(serialize(p.second));
            }
            return res;
        }

        /**
         * @brief 序列化 unordered_map<string, string> 到字节流
         */
        static std::string serialize(const std::unordered_map<std::string, std::string> &map)
        {
            std::string result;
            uint32_t size = htonl(static_cast<uint32_t>(map.size()));
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &[key, value] : map)
            {
                result.append(serialize(key));
                result.append(serialize(value));
            }
            return result;
        }
        /**
         * @brief 序列化 SkipList<std::string, std::string> 到字节流
         */
        static std::string serialize(const SkipList<std::string, std::string> &skiplist)
        {
            return skiplist.serialize(serialize, serialize);
        }

        /**
         * @brief 序列化 ZSet 到字节流
         */
        static std::string serialize(const ZSet &zset)
        {
            auto sl = [](const SkipList<ZSetKey, std::string> &s)
            {
                return serialize(s);
            };
            auto sm = [](const std::unordered_map<std::string, double> &map)
            {
                return serialize(map);
            };
            return zset.serialize(sl, sm);
        }
        // 从字节流反序列化

        /**
         * @brief 从字节流反序列化 string
         * @param data 数据指针
         * @param offset 当前偏移量（会被更新）
         * @return 反序列化的字符串
         */
        static std::string deserializeString(const char *data, size_t &offset)
        {
            uint32_t len;
            std::memcpy(&len, data + offset, sizeof(len));
            offset += sizeof(len);
            len = ntohl(len);
            std::string result(data + offset, len);
            offset += len;
            return result;
        }

        /**
         * @brief 从字节流反序列化 vector<string>
         */
        static void deserializeVector(const char *data, size_t &offset, std::vector<std::string> &vec)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            vec.clear();
            vec.reserve(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                vec.push_back(deserializeString(data, offset));
            }
        }

        /**
         * @brief 从字节流反序列化 vector<pair<string, string>>
         */
        static void deserializeVector(const char *data, size_t &offset, std::vector<std::pair<std::string, std::string>> &vec)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            vec.clear();
            vec.reserve(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                std::string key = deserializeString(data, offset);
                std::string value = deserializeString(data, offset);
                vec.emplace_back(std::move(key), std::move(value));
            }
        }

        /**
         * @brief 从字节流反序列化deque<string>
         */
        static void deserializeDeque(const char *data, size_t &offset, std::deque<std::string> &deque)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            deque.clear();
            for (uint32_t i = 0; i < size; ++i)
            {
                deque.push_back(deserializeString(data, offset));
            }
        }

        /**
         * @brief 从字节流反序列化 unordered_set<string>
         */
        static void deserializeSet(const char *data, size_t &offset, std::unordered_set<std::string> &set)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            set.clear();
            set.reserve(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                set.insert(deserializeString(data, offset));
            }
        }

        /**
         * @brief 从字节流反序列化 unordered_map<string, string>
         */
        static void deserializeMap(const char *data, size_t &offset, std::unordered_map<std::string, std::string> &map)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);
            size = ntohl(size);
            map.clear();
            map.reserve(size);
            for (uint32_t i = 0; i < size; ++i)
            {
                std::string key = deserializeString(data, offset);
                std::string value = deserializeString(data, offset);
                map.emplace(std::move(key), std::move(value));
            }
        }

        /**
         * @brief 从字节流跳过指定大小的数据
         */
        static void deserializeSkipList(const char *data, size_t &offset, SkipList<std::string, std::string> &skiplist)
        {
            skiplist.deserialize(data, offset, deserializeString, deserializeString);
        }

        static double deserializeDouble(const char *data, size_t &offset)
        {
            double v;
            std::memcpy(&v, data + offset, sizeof(double));
            offset += sizeof(double);
            return v;
        }

        static ZSetKey deserializeZSetKey(const char *data, size_t &offset)
        {
            double s = deserializeDouble(data, offset);
            std::string m = deserializeString(data, offset);
            return {s, m};
        }

        static void deserializeZSet(const char *data, size_t &offset, ZSet &zset)
        {
            auto ds_sl = [](const char *d, size_t &o, SkipList<ZSetKey, std::string> &sl)
            {
                // 适配 SkipList.deserialize 签名
                // 注意：SkipList 的定义中 deserialize 需要返回 K 的函数指针
                auto k_func = [](const char *dd, size_t &oo)
                { return Serializer::deserializeZSetKey(dd, oo); };
                auto v_func = [](const char *dd, size_t &oo)
                { return Serializer::deserializeString(dd, oo); };
                sl.deserialize(d, o, k_func, v_func);
            };

            auto ds_map = [](const char *d, size_t &o, std::unordered_map<std::string, double> &map)
            {
                uint32_t size;
                std::memcpy(&size, d + o, sizeof(size));
                o += sizeof(size);
                size = ntohl(size);
                for (uint32_t i = 0; i < size; ++i)
                {
                    std::string k = deserializeString(d, o);
                    double v = deserializeDouble(d, o);
                    map[k] = v;
                }
            };

            zset.deserialize(data, offset, ds_sl, ds_map);
        }
    };

} // namespace serialize

typedef EyaKV::Serializer Serializer;

#endif // TINYKV_INCLUDE_COMMON_SERIALIZE_H_