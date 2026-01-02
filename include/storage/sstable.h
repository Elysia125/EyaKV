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
#include "config/config.h"
#include "storage/node.h"
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
constexpr size_t SSTABLE_BLOCK_SIZE = 4 * 1024;         // 数据块大小 4KB
constexpr size_t SSTABLE_BLOOM_BITS_PER_KEY = 10;       // 布隆过滤器每个key使用的位数
constexpr uint64_t SSTABLE_MAGIC_NUMBER = 0x4579614B56; // SSTable 魔数

/**
 * @brief SSTable Footer 结构
 * 位于文件末尾，存储元数据
 */
struct SSTableFooter
{
    uint64_t index_block_offset;  // 索引块在文件中的偏移
    uint64_t index_block_size;    // 索引块大小
    uint64_t bloom_filter_offset; // 布隆过滤器偏移
    uint64_t bloom_filter_size;   // 布隆过滤器大小
    uint64_t data_block_count;    // 数据块数量
    uint64_t entry_count;         // KV对总数
    uint64_t min_key_offset;      // 最小key在文件中的偏移
    uint64_t min_key_size;        // 最小key的长度
    uint64_t max_key_offset;      // 最大key在文件中的偏移
    uint64_t max_key_size;        // 最大key的长度
    uint64_t magic;               // 魔数，用于验证文件格式

    // 序列化到字节流
    std::string serialize() const;
    // 从字节流反序列化
    static SSTableFooter deserialize(const char *data);
    // Footer固定大小
    static constexpr size_t SIZE = sizeof(uint64_t) * 11;
};

/**
 * @brief 索引条目，存储数据块的信息
 */
struct IndexEntry
{
    std::string first_key; // 数据块的第一个key
    uint64_t block_offset; // 数据块在文件中的偏移
    uint64_t block_size;   // 数据块大小

    // 序列化到字节流
    std::string serialize() const;
    // 从字节流反序列化
    static IndexEntry deserialize(const char *data, size_t &offset);
};

/**
 * @brief 简单的布隆过滤器实现
 */
class BloomFilter
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

/**
 * @brief SSTable 元数据，用于管理多个SSTable
 */
struct SSTableMeta
{
    std::string filepath;     // SSTable文件路径
    std::string min_key;      // 最小key
    std::string max_key;      // 最大key
    uint64_t file_size;       // 文件大小
    uint64_t entry_count;     // KV对数量
    uint32_t level;           // 所在层级(用于LSM-tree压缩)
    uint64_t sequence_number; // 序列号(用于排序和版本控制)

    // 检查key是否可能在这个SSTable的范围内
    bool may_contain_key(const std::string &key) const
    {
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
    SSTable(const SSTable &) = delete;
    SSTable &operator=(const SSTable &) = delete;

    // 允许移动
    SSTable(SSTable &&) = default;
    SSTable &operator=(SSTable &&) = default;

    /**
     * @brief 从 SSTable 中查找 Key。
     * @param key 要查找的key
     * @return 如果找到返回对应的value，否则返回空
     */
    std::optional<EValue> get(const std::string &key) const;

    /**
     * @brief 检查key是否可能存在（使用布隆过滤器）
     * @param key 要检查的key
     * @return 如果可能存在返回true（可能有假阳性）
     */
    bool may_contain(const std::string &key) const;

    /**
     * @brief 获取SSTable的元数据
     */
    const SSTableMeta &get_meta() const { return meta_; }

    /**
     * @brief 获取SSTable的文件路径
     */
    const std::string &get_filepath() const { return filepath_; }

    /**
     * @brief 获取KV对数量
     */
    size_t get_entry_count() const { return footer_.entry_count; }

    /**
     * @brief 范围查询 - 获取指定范围内的所有KV对
     * @param start_key 起始key（包含）
     * @param end_key 结束key（包含）
     * @return 范围内的KV对列表
     */
    std::vector<std::pair<std::string, EValue>> range(
        const std::string &start_key,
        const std::string &end_key) const;

    /**
     * @brief 遍历SSTable中的所有KV对
     * @param callback 回调函数，返回false时停止遍历
     */
    void for_each(const std::function<bool(const std::string &, const EValue &)> &callback) const;

    std::map<std::string, EValue> range_map(
        const std::string &start_key,
        const std::string &end_key) const;

private:
    std::string filepath_;
    mutable FILE *file_;
    SSTableFooter footer_;
    std::vector<IndexEntry> index_;
    BloomFilter bloom_filter_;
    SSTableMeta meta_;

    /**
     * @brief 从指定的数据块中读取所有 KV 对。
     * @param block_index 数据块在 index_ 中的索引
     * @return 该数据块包含的所有 KV 对
     */
    std::vector<std::pair<std::string, EValue>> read_data_block(size_t block_index) const;

    /**
     * @brief 在内存中的数据块内进行二分查找。
     * @param block 已加载到内存的数据块 KV 对列表
     * @param key 要查找的 key
     * @return 如果找到返回对应的 value，否则返回 std::nullopt
     */
    std::optional<EValue> search_in_block(
        const std::vector<std::pair<std::string, EValue>> &block,
        const std::string &key) const;

    /**
     * @brief 通过索引查找可能包含 key 的数据块索引。
     * 使用二分查找在 IndexBlock 中定位。
     * @param key 要查找的 key
     * @return 可能包含该 key 的数据块索引
     */
    size_t find_block_index(const std::string &key) const;

    /**
     * @brief 加载 SSTable 的索引、BloomFilter 和元数据。
     * 在构造时调用，只读取元数据部分，不加载实际数据块。
     * @return 加载成功返回 true
     */
    bool load();
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
                            const uint32_t level = 0,
                            size_t block_size = SSTABLE_BLOCK_SIZE);
    ~SSTableBuilder();

    // 禁止拷贝
    SSTableBuilder(const SSTableBuilder &) = delete;
    SSTableBuilder &operator=(const SSTableBuilder &) = delete;

    /**
     * @brief 添加 Key-Value 对。必须按 Key 升序添加。
     * @param key 键
     * @param value 值
     */
    void add(const std::string &key, const EValue &value);

    /**
     * @brief 完成构建并关闭文件。
     * @return 构建成功返回true
     */
    bool finish();

    /**
     * @brief 获取已添加的KV对数量
     */
    size_t get_entry_count() const { return entry_count_; }

    /**
     * @brief 获取当前文件大小估算
     */
    size_t get_file_size() const { return current_offset_; }

    /**
     * @brief 取消构建，删除临时文件
     */
    void abort();

private:
    std::string filepath_;
    const uint32_t level_;
    FILE *file_;
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

    /**
     * @brief 将当前数据块刷新到文件。
     * 会同时更新 IndexBlock 信息。
     */
    void flush_block();

    /**
     * @brief 写入 Footer 信息到文件末尾。
     * 包括 IndexBlock 偏移、BloomFilter 偏移等。
     */
    void write_footer();

    /**
     * @brief 序列化单个 KV 对。
     * 格式：[key_len][key][value_len][value]
     */
    static std::string serialize_entry(const std::string &key, const EValue &value);
};

/**
 * @brief SSTable 管理器，负责管理多个SSTable文件
 */
class SSTableManager
{
public:
    explicit SSTableManager(const std::string &data_dir,
                            const SSTableMergeStrategy &merge_strategy,
                            const uint32_t &sstable_merge_threshold,
                            const uint64_t &sstable_zero_level_size,
                            const double &sstable_level_size_ratio);
    ~SSTableManager() = default;

    /**
     * @brief 从MemTable创建新的SSTable
     * @param entries MemTable中的所有条目（按key排序）
     * @return 新创建的SSTable的元数据
     */
    std::optional<SSTableMeta> create_new_sstable(const std::vector<std::pair<std::string, EValue>> &entries);
    /**
     * @brief 在所有SSTable中查找key
     * @param key 要查找的key
     * @param value 如果找到，存储对应的value
     * @return 如果找到返回true
     */
    bool get(const std::string &key, EValue *value) const;

    /**
     * @brief 获取所有SSTable的数量
     */
    size_t get_sstable_count() const { return sstable_count_; }

    /**
     * @brief 获取所有SSTable的总大小
     */
    size_t get_total_size() const;

    /**
     * @brief 范围查询
     */
    std::map<std::string, EValue> range_query(
        const std::string &start_key,
        const std::string &end_key) const;

private:
    std::string data_dir_;
    uint32_t sstable_count_;
    std::vector<std::vector<std::unique_ptr<SSTable>>> level_sstables_;
    std::vector<uint64_t> level_sstable_size_;
    std::vector<std::unique_ptr<std::recursive_mutex>> level_mutex_;
    uint32_t max_level_;
    SSTableMergeStrategy merge_strategy_;
    uint64_t sstable_zero_level_size_; // bit
    double sstable_level_size_ratio_;
    uint64_t next_sequence_number_;
    uint32_t sstable_merge_threshold_;
    /**
     * @brief 生成唯一的 SSTable 文件名。
     * 格式：[sequence_number].sst
     */
    std::string generate_filename();

    /**
     * @brief 按照序列号对 SSTable 进行排序。
     * 序列号大的（新的）排在前面，优化查询路径。
     */
    void sort_sstables_by_sequence();

    /**
     * @brief 触发指定层级的合并操作（通用入口）。
     * 根据当前的合并策略分发到具体的实现函数。
     * @param level 要进行合并的层级
     * @return 合并成功返回 true
     */
    bool merge_sstables(const uint32_t level);

    /**
     * @brief 使用策略0 (Size-Tiered) 合并指定层级。
     * 当某层 SSTable 数量超过阈值时触发。
     */
    bool merge_sstables_by_strategy_0(const uint32_t level);

    /**
     * @brief 使用策略1 (Leveled) 合并指定层级。
     * 当某层总大小超过限制时触发。
     */
    bool merge_sstables_by_strategy_1(const uint32_t level);

    /**
     * @brief 从内存中的 entries 创建新的 SSTable。
     * 同时也负责更新内存中的 SSTable 列表。
     * @param entries 有序的 KV 对列表
     * @param level 新 SSTable 所属的层级
     * @return 新建 SSTable 的元数据
     */
    std::optional<SSTableMeta> create_from_entries(
        const std::vector<std::pair<std::string, EValue>> &entries, const uint32_t level = 0);

    /**
     * @brief 加载数据目录下的所有 SSTable 文件。
     * 启动时调用，恢复内存索引结构。
     * @return 加载成功返回 true
     */
    bool load_all();

    /**
     * @brief 规范化 SSTable 状态。
     * 检查并处理不符合当前合并策略的残留文件或状态。
     */
    void normalize_sstables();
};

#endif // SSTABLE_H_
