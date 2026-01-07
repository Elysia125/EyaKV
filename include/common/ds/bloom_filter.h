#ifndef EYAKV_COMMON_BLOOM_FILTER_H_
#define EYAKV_COMMON_BLOOM_FILTER_H_

#include <cstdint>
#include <string>
#include <vector>
#include "common/base/export.h"

constexpr size_t SSTABLE_BLOOM_BITS_PER_KEY = 10; // 布隆过滤器每个key使用的位数
/**
 * @brief 简单的布隆过滤器实现
 */
class EYAKV_COMMON_API BloomFilter
{
public:
    BloomFilter();
    explicit BloomFilter(size_t expected_elements, size_t bits_per_key = SSTABLE_BLOOM_BITS_PER_KEY);

    // 添加一个key到布隆过滤器
    void add(const std::string &key);

    // 检查key是否可能存在（可能有假阳性，但无假阴性）
    bool may_contain(const std::string &key) const;

    // 序列化到字节流
    std::string serialize() const;

    // 从字节流反序列化
    static BloomFilter deserialize(const char *data, size_t &offset);

    // 获取位数组大小
    size_t size() const { return bits_.size(); }

private:
    std::vector<uint8_t> bits_;
    size_t num_hash_functions_;

    // 计算多个hash值
    std::vector<uint32_t> get_hashes(const std::string &key) const;
};

#endif // EYAKV_COMMON_BLOOM_FILTER_H_