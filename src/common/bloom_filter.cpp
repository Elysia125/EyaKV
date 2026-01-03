#include "common/bloom_filter.h"
#include <cstring>

BloomFilter::BloomFilter() : num_hash_functions_(0) {}

BloomFilter::BloomFilter(size_t expected_elements, size_t bits_per_key)
    : num_hash_functions_(std::min(static_cast<size_t>(30),
                                   std::max(static_cast<size_t>(1),
                                            static_cast<size_t>(bits_per_key * 0.69)))) // ln(2) ≈ 0.69
{
    size_t total_bits = expected_elements * bits_per_key;
    size_t num_bytes = (total_bits + 7) / 8; // 向上取整,避免丢失字节
    bits_.resize(num_bytes, 0);
}

std::vector<uint32_t> BloomFilter::get_hashes(const std::string &key) const
{
    std::vector<uint32_t> hashes;
    hashes.reserve(num_hash_functions_);

    // 使用 MurmurHash3 的简化版本计算两个基础哈希
    uint32_t h1 = 0;
    uint32_t h2 = 0;

    for (size_t i = 0; i < key.size(); ++i)
    {
        h1 = h1 * 31 + static_cast<uint8_t>(key[i]);
        h2 = h2 * 37 + static_cast<uint8_t>(key[i]);
    }

    // 使用双哈希技术生成多个哈希值
    for (size_t i = 0; i < num_hash_functions_; ++i)
    {
        hashes.push_back(h1 + i * h2);
    }

    return hashes;
}

void BloomFilter::add(const std::string &key)
{
    if (bits_.empty())
        return;

    size_t num_bits = bits_.size() * 8;
    auto hashes = get_hashes(key);

    for (uint32_t hash : hashes)
    {
        size_t bit_pos = hash % num_bits;
        bits_[bit_pos / 8] |= (1 << (bit_pos % 8));
    }
}

bool BloomFilter::may_contain(const std::string &key) const
{
    if (bits_.empty())
        return true;

    size_t num_bits = bits_.size() * 8;
    auto hashes = get_hashes(key);

    for (uint32_t hash : hashes)
    {
        size_t bit_pos = hash % num_bits;
        if (!(bits_[bit_pos / 8] & (1 << (bit_pos % 8))))
        {
            return false;
        }
    }
    return true;
}

std::string BloomFilter::serialize() const
{
    std::string result;

    // 写入哈希函数数量
    uint32_t num_hashes = static_cast<uint32_t>(num_hash_functions_);
    result.append(reinterpret_cast<const char *>(&num_hashes), sizeof(num_hashes));

    // 写入位数组大小和内容
    uint32_t bits_size = static_cast<uint32_t>(bits_.size());
    result.append(reinterpret_cast<const char *>(&bits_size), sizeof(bits_size));
    result.append(reinterpret_cast<const char *>(bits_.data()), bits_.size());

    return result;
}

BloomFilter BloomFilter::deserialize(const char *data, size_t &offset)
{
    BloomFilter filter;

    // 读取哈希函数数量
    uint32_t num_hashes;
    std::memcpy(&num_hashes, data + offset, sizeof(num_hashes));
    offset += sizeof(num_hashes);
    filter.num_hash_functions_ = num_hashes;

    // 读取位数组
    uint32_t bits_size;
    std::memcpy(&bits_size, data + offset, sizeof(bits_size));
    offset += sizeof(bits_size);

    filter.bits_.resize(bits_size);
    std::memcpy(filter.bits_.data(), data + offset, bits_size);

    return filter;
}