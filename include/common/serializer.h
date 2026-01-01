#ifndef TINYKV_INCLUDE_COMMON_SERIALIZE_H_
#define TINYKV_INCLUDE_COMMON_SERIALIZE_H_

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include "skip_list.h"
#include "zset.h"

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
        // ==================== 序列化到字节流 ====================

        /**
         * @brief 序列化 string 到字节流
         */
        static std::string serialize(const std::string &str)
        {
            std::string result;
            uint32_t len = static_cast<uint32_t>(str.size());
            result.append(reinterpret_cast<const char *>(&len), sizeof(len));
            result.append(str);
            return result;
        }

        static std::string serialize(const double &val)
        {
            std::string result;
            result.append(reinterpret_cast<const char *>(&val), sizeof(val));
            return result;
        }

        /**
         * @brief 序列化 vector<string> 到字节流
         */
        static std::string serialize(const std::vector<std::string> &vec)
        {
            std::string result;
            uint32_t size = static_cast<uint32_t>(vec.size());
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
            uint32_t size = static_cast<uint32_t>(vec.size());
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
            uint32_t size = static_cast<uint32_t>(deque.size());
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
            uint32_t size = static_cast<uint32_t>(set.size());
            result.append(reinterpret_cast<const char *>(&size), sizeof(size));
            for (const auto &str : set)
            {
                result.append(serialize(str));
            }
            return result;
        }

        /**
         * @brief 序列化 unordered_map<string, string> 到字节流
         */
        static std::string serialize(const std::unordered_map<std::string, std::string> &map)
        {
            std::string result;
            uint32_t size = static_cast<uint32_t>(map.size());
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
            return zset.serialize(serialize, serialize);
        }
        // ==================== 从字节流反序列化 ====================

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
            std::string result(data + offset, len);
            offset += len;
            return result;
        }

        /**
         * @brief 从字节流反序列化 double
         */
        static double deserializeDouble(const char *data, size_t &offset)
        {
            double val;
            std::memcpy(&val, data + offset, sizeof(val));
            offset += sizeof(val);
            return val;
        }

        /**
         * @brief 从字节流反序列化 vector<string>
         */
        static void deserializeVector(const char *data, size_t &offset, std::vector<std::string> &vec)
        {
            uint32_t size;
            std::memcpy(&size, data + offset, sizeof(size));
            offset += sizeof(size);

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

        /**
         * @brief 从字节流反序列化 ZSet
         */
        static void deserializeZSet(const char *data, size_t &offset, ZSet &zset)
        {
            zset.deserialize(data, offset, deserializeSkipList, deserializeMap);
        }
    };

} // namespace serialize

typedef EyaKV::Serializer Serializer;

#endif // TINYKV_INCLUDE_COMMON_SERIALIZE_H_