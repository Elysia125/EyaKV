#ifndef SSTABLE_H_
#define SSTABLE_H_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <fstream>
#include <optional>
#include <cstdint>
#include <functional>
#include "common/common.h"

/**
 * @brief SSTable 文件格式:
 * 
 * +-------------------+
 * |   Data Block 1    |  <- 多个数据块，每个块包含多个KV对
 * +-------------------+
 * |   Data Block 2    |
 * +-------------------+
 * |       ...         |
 * +-------------------+
 * |   Data Block N    |
 * +-------------------+
 * |   Index Block     |  <- 存储每个数据块的起始key和偏移量
 * +-------------------+
 * |  Bloom Filter     |  <- 布隆过滤器，快速判断key是否存在
 * +-------------------+
 * |     Footer        |  <- 元数据：索引块偏移、布隆过滤器偏移等
 * +-------------------+
 */

// SSTable 常量定义
constexpr size_t SSTABLE_BLOCK_SIZE = 4 * 1024;        // 数据块大小 4KB
constexpr size_t SSTABLE_BLOOM_BITS_PER_KEY = 10;      // 布隆过滤器每个key使用的位数
constexpr uint32_t SSTABLE_MAGIC_NUMBER = 0x54535354;  // "TSST" - TinyKV SSTable

/**
 * @brief SSTable Footer 结构
 * 位于文件末尾，存储元数据
 */
struct SSTableFooter {
    uint64_t index_block_offset;      // 索引块在文件中的偏移
    uint64_t index_block_size;        // 索引块大小
    uint64_t bloom_filter_offset;     // 布隆过滤器偏移
    uint64_t bloom_filter_size;       // 布隆过滤器大小
    uint64_t data_block_count;        // 数据块数量
    uint64_t entry_count;             // KV对总数
    uint64_t min_key_offset;          // 最小key在文件中的偏移
    uint64_t min_key_size;            // 最小key的长度
    uint64_t max_key_offset;          // 最大key在文件中的偏移
    uint64_t max_key_size;            // 最大key的长度
    uint32_t magic;                   // 魔数，用于验证文件格式

    // 序列化到字节流
    std::string serialize() const;
    // 从字节流反序列化
    static SSTableFooter deserialize(const char* data);
    // Footer固定大小
    static constexpr size_t SIZE = sizeof(uint64_t) * 10 + sizeof(uint32_t);
};

/**
 * @brief 索引条目，存储数据块的信息
 */
struct IndexEntry {
    std::string first_key;       // 数据块的第一个key
    uint64_t block_offset;       // 数据块在文件中的偏移
    uint64_t block_size;         // 数据块大小

    // 序列化到字节流
    std::string serialize() const;
    // 从字节流反序列化
    static IndexEntry deserialize(const char* data, size_t& offset);
};

/**
 * @brief 简单的布隆过滤器实现
 */
class BloomFilter {
public:
    BloomFilter();
    explicit BloomFilter(size_t expected_elements, size_t bits_per_key = SSTABLE_BLOOM_BITS_PER_KEY);

    // 添加一个key到布隆过滤器
    void add(const std::string& key);

    // 检查key是否可能存在（可能有假阳性，但无假阴性）
    bool mayContain(const std::string& key) const;

    // 序列化到字节流
    std::string serialize() const;

    // 从字节流反序列化
    static BloomFilter deserialize(const char* data, size_t size);

    // 获取位数组大小
    size_t size() const { return bits_.size(); }

private:
    std::vector<uint8_t> bits_;
    size_t num_hash_functions_;

    // 计算多个hash值
    std::vector<uint32_t> getHashes(const std::string& key) const;
};

/**
 * @brief SSTable 元数据，用于管理多个SSTable
 */
struct SSTableMeta {
    std::string filepath;         // SSTable文件路径
    std::string min_key;          // 最小key
    std::string max_key;          // 最大key
    uint64_t file_size;           // 文件大小
    uint64_t entry_count;         // KV对数量
    uint32_t level;               // 所在层级(用于LSM-tree压缩)
    uint64_t sequence_number;     // 序列号(用于排序和版本控制)

    // 检查key是否可能在这个SSTable的范围内
    bool mayContainKey(const std::string& key) const {
        return key >= min_key && key <= max_key;
    }
};

/**
 * @brief SSTable (Sorted String Table) 是磁盘上的不可变有序文件。
 *
 * 当 MemTable 达到一定大小时，会 Flush 到磁盘生成 SSTable。
 * SSTable 内部按 key 有序存储，支持高效的点查询和范围查询。
 */
class SSTable
{
public:
    /**
     * @brief 从文件加载 SSTable
     * @param filepath SSTable文件路径
     */
    explicit SSTable(const std::string &filepath);
    ~SSTable();

    // 禁止拷贝
    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;

    // 允许移动
    SSTable(SSTable&&) = default;
    SSTable& operator=(SSTable&&) = default;

    /**
     * @brief 从 SSTable 中查找 Key。
     * @param key 要查找的key
     * @param value 如果找到，存储对应的value
     * @return 如果找到返回true，否则返回false
     */
    bool Get(const std::string &key, EyaValue *value) const;

    /**
     * @brief 检查key是否可能存在（使用布隆过滤器）
     * @param key 要检查的key
     * @return 如果可能存在返回true（可能有假阳性）
     */
    bool MayContain(const std::string& key) const;

    /**
     * @brief 获取SSTable的元数据
     */
    const SSTableMeta& GetMeta() const { return meta_; }

    /**
     * @brief 获取SSTable的文件路径
     */
    const std::string& GetFilepath() const { return filepath_; }

    /**
     * @brief 获取KV对数量
     */
    size_t GetEntryCount() const { return footer_.entry_count; }

    /**
     * @brief 范围查询 - 获取指定范围内的所有KV对
     * @param start_key 起始key（包含）
     * @param end_key 结束key（包含）
     * @return 范围内的KV对列表
     */
    std::vector<std::pair<std::string, EyaValue>> Range(
        const std::string& start_key, 
        const std::string& end_key) const;

    /**
     * @brief 遍历SSTable中的所有KV对
     * @param callback 回调函数，返回false时停止遍历
     */
    void ForEach(const std::function<bool(const std::string&, const EyaValue&)>& callback) const;

private:
    std::string filepath_;
    mutable std::ifstream file_;
    SSTableFooter footer_;
    std::vector<IndexEntry> index_;
    BloomFilter bloom_filter_;
    SSTableMeta meta_;

    // 读取指定数据块
    std::vector<std::pair<std::string, EyaValue>> ReadDataBlock(size_t block_index) const;

    // 在数据块内二分查找
    std::optional<EyaValue> SearchInBlock(
        const std::vector<std::pair<std::string, EyaValue>>& block,
        const std::string& key) const;

    // 通过索引找到可能包含key的数据块
    size_t FindBlockIndex(const std::string& key) const;

    // 加载SSTable的索引和元数据
    bool Load();
};

/**
 * @brief 用于构建 SSTable 的辅助类。
 * 
 * 使用方法：
 * 1. 创建 SSTableBuilder
 * 2. 按 Key 升序调用 Add 添加数据
 * 3. 调用 Finish 完成构建
 */
class SSTableBuilder
{
public:
    /**
     * @brief 创建 SSTableBuilder
     * @param filepath 输出文件路径
     * @param block_size 数据块大小（默认4KB）
     */
    explicit SSTableBuilder(const std::string &filepath, 
                           size_t block_size = SSTABLE_BLOCK_SIZE);
    ~SSTableBuilder();

    // 禁止拷贝
    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    /**
     * @brief 添加 Key-Value 对。必须按 Key 升序添加。
     * @param key 键
     * @param value 值
     */
    void Add(const std::string &key, const EyaValue &value);

    /**
     * @brief 完成构建并关闭文件。
     * @return 构建成功返回true
     */
    bool Finish();

    /**
     * @brief 获取已添加的KV对数量
     */
    size_t GetEntryCount() const { return entry_count_; }

    /**
     * @brief 获取当前文件大小估算
     */
    size_t GetFileSize() const { return current_offset_; }

    /**
     * @brief 获取构建后的SSTable元数据
     * （调用Finish后有效）
     */
    SSTableMeta GetMeta() const;

    /**
     * @brief 取消构建，删除临时文件
     */
    void Abort();

private:
    std::string filepath_;
    std::ofstream file_;
    size_t block_size_;
    size_t entry_count_;
    size_t current_offset_;

    // 当前数据块的缓冲区
    std::string current_block_;
    std::string first_key_in_block_;
    size_t entries_in_block_;

    // 索引条目
    std::vector<IndexEntry> index_entries_;

    // 布隆过滤器
    std::unique_ptr<BloomFilter> bloom_filter_;

    // 记录min/max key
    std::string min_key_;
    std::string max_key_;

    bool finished_;
    bool aborted_;

    // 刷新当前数据块到文件
    void FlushBlock();

    // 写入索引块
    void WriteIndexBlock();

    // 写入布隆过滤器
    void WriteBloomFilter();

    // 写入Footer
    void WriteFooter();

    // 序列化单个KV对
    static std::string SerializeEntry(const std::string& key, const EyaValue& value);
};

/**
 * @brief SSTable 管理器，负责管理多个SSTable文件
 */
class SSTableManager {
public:
    explicit SSTableManager(const std::string& data_dir);
    ~SSTableManager() = default;

    /**
     * @brief 从MemTable创建新的SSTable
     * @param entries MemTable中的所有条目（按key排序）
     * @return 新创建的SSTable的元数据
     */
    std::optional<SSTableMeta> CreateFromEntries(
        const std::vector<std::pair<std::string, EyaValue>>& entries);

    /**
     * @brief 在所有SSTable中查找key
     * @param key 要查找的key
     * @param value 如果找到，存储对应的value
     * @return 如果找到返回true
     */
    bool Get(const std::string& key, EyaValue* value) const;

    /**
     * @brief 加载数据目录下的所有SSTable
     * @return 加载成功返回true
     */
    bool LoadAll();

    /**
     * @brief 获取所有SSTable的数量
     */
    size_t GetSSTableCount() const { return sstables_.size(); }

    /**
     * @brief 获取所有SSTable的总大小
     */
    size_t GetTotalSize() const;

private:
    std::string data_dir_;
    std::vector<std::unique_ptr<SSTable>> sstables_;
    uint64_t next_sequence_number_;

    // 生成新的SSTable文件名
    std::string GenerateFilename();

    // 按照查询顺序排序SSTable（最新的在前）
    void SortSSTablesBySequence();
};

#endif // SSTABLE_H_

