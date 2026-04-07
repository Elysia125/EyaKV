#include "storage/sstable.h"
#include "logger/logger.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
// Windows 下模拟 pread，实现线程安全的原子读取，解除并发时的 fseek 游标竞争
inline bool pread_exact(FILE *file, void *buffer, size_t size, uint64_t offset)
{
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    OVERLAPPED overlapped = {0};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesRead = 0;
    return ReadFile(hFile, buffer, static_cast<DWORD>(size), &bytesRead, &overlapped) && bytesRead == size;
}
#else
#include <unistd.h>
// Linux/macOS 的原生原子读取，天生无锁
inline bool pread_exact(FILE *file, void *buffer, size_t size, uint64_t offset)
{
    return pread(fileno(file), buffer, size, offset) == (ssize_t)size;
}
#endif
// SSTableFooter 实现

std::string SSTableFooter::serialize() const
{
    std::string result;
    result.reserve(SIZE);

    auto append_uint64 = [&result](uint64_t val)
    {
        result.append(reinterpret_cast<const char *>(&val), sizeof(val));
    };

    append_uint64(index_block_offset);
    append_uint64(index_block_size);
    append_uint64(bloom_filter_offset);
    append_uint64(bloom_filter_size);
    append_uint64(data_block_count);
    append_uint64(entry_count);
    append_uint64(min_key_offset);
    append_uint64(min_key_size);
    append_uint64(max_key_offset);
    append_uint64(max_key_size);
    append_uint64(magic);

    return result;
}

SSTableFooter SSTableFooter::deserialize(const char *data)
{
    SSTableFooter footer;
    size_t offset = 0;

    auto read_uint64 = [&data, &offset]() -> uint64_t
    {
        uint64_t val;
        std::memcpy(&val, data + offset, sizeof(val));
        offset += sizeof(val);
        return val;
    };

    footer.index_block_offset = read_uint64();
    footer.index_block_size = read_uint64();
    footer.bloom_filter_offset = read_uint64();
    footer.bloom_filter_size = read_uint64();
    footer.data_block_count = read_uint64();
    footer.entry_count = read_uint64();
    footer.min_key_offset = read_uint64();
    footer.min_key_size = read_uint64();
    footer.max_key_offset = read_uint64();
    footer.max_key_size = read_uint64();
    footer.magic = read_uint64();

    return footer;
}

// IndexEntry 实现

std::string IndexEntry::serialize() const
{
    std::string result;
    // 预分配规避内存碎片
    result.reserve(sizeof(uint32_t) + first_key.size() + sizeof(block_offset) + sizeof(block_size));
    // 写入 first_key 长度和内容
    uint32_t key_len = static_cast<uint32_t>(first_key.size());
    result.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
    result.append(first_key);

    // 写入 block_offset 和 block_size
    result.append(reinterpret_cast<const char *>(&block_offset), sizeof(block_offset));
    result.append(reinterpret_cast<const char *>(&block_size), sizeof(block_size));

    return result;
}

IndexEntry IndexEntry::deserialize(const char *data, size_t &offset)
{
    IndexEntry entry;

    // 读取 first_key
    uint32_t key_len;
    std::memcpy(&key_len, data + offset, sizeof(key_len));
    offset += sizeof(key_len);
    entry.first_key = std::string(data + offset, key_len);
    offset += key_len;

    // 读取 block_offset 和 block_size
    std::memcpy(&entry.block_offset, data + offset, sizeof(entry.block_offset));
    offset += sizeof(entry.block_offset);
    std::memcpy(&entry.block_size, data + offset, sizeof(entry.block_size));
    offset += sizeof(entry.block_size);

    return entry;
}

// SSTable 实现
SSTable::SSTable(const std::string &filepath, std::shared_ptr<BlockCache> block_cache)
    : filepath_(filepath), block_cache_(block_cache)
{
    if (!load())
    {
        LOG_ERROR("Failed to load SSTable: {}", filepath.c_str());
    }
}

SSTable::~SSTable()
{
    if (file_ != nullptr)
    {
        LOG_INFO("SSTable::~SSTable: Closing file {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        LOG_INFO("SSTable::~SSTable: File {} closed", filepath_.c_str());
    }
}

bool SSTable::load()
{
    file_ = fopen(filepath_.c_str(), "rb");
    if (file_ == nullptr)
    {
        LOG_ERROR("Cannot open SSTable file: {}", filepath_.c_str());
        return false;
    }

    // 获取文件大小
    fseek(file_, 0, SEEK_END);
    size_t file_size = ftell(file_);

    if (file_size < SSTableFooter::SIZE)
    {
        LOG_ERROR("SSTable file too small: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    // 读取level
    uint32_t level;
    fseek(file_, file_size - sizeof(level), SEEK_SET);
    if (fread(&level, 1, sizeof(level), file_) != sizeof(level))
    {
        LOG_ERROR("Failed to read level from SSTable: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    // 读取 Footer
    fseek(file_, file_size - sizeof(level) - SSTableFooter::SIZE, SEEK_SET);
    std::vector<char> footer_data(SSTableFooter::SIZE);
    if (fread(footer_data.data(), 1, SSTableFooter::SIZE, file_) != SSTableFooter::SIZE)
    {
        LOG_ERROR("Failed to read footer from SSTable: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    footer_ = SSTableFooter::deserialize(footer_data.data());

    // 验证魔数
    if (footer_.magic != SSTABLE_MAGIC_NUMBER)
    {
        LOG_ERROR("Invalid SSTable magic number: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    // 读取 min_key 和 max_key
    std::string min_key, max_key;
    if (footer_.min_key_size > 0)
    {
        fseek(file_, footer_.min_key_offset, SEEK_SET);
        min_key.resize(footer_.min_key_size);
        if (fread(&min_key[0], 1, footer_.min_key_size, file_) != footer_.min_key_size)
        {
            LOG_ERROR("Failed to read min_key from SSTable: {}", filepath_.c_str());
            fclose(file_);
            file_ = nullptr;
            return false;
        }
    }
    if (footer_.max_key_size > 0)
    {
        fseek(file_, footer_.max_key_offset, SEEK_SET);
        max_key.resize(footer_.max_key_size);
        if (fread(&max_key[0], 1, footer_.max_key_size, file_) != footer_.max_key_size)
        {
            LOG_ERROR("Failed to read max_key from SSTable: {}", filepath_.c_str());
            fclose(file_);
            file_ = nullptr;
            return false;
        }
    }

    // 读取索引块
    fseek(file_, footer_.index_block_offset, SEEK_SET);
    std::vector<char> index_data(footer_.index_block_size);
    if (fread(index_data.data(), 1, footer_.index_block_size, file_) != footer_.index_block_size)
    {
        LOG_ERROR("Failed to read index block from SSTable: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    size_t offset = 0;
    while (offset < footer_.index_block_size)
    {
        index_.push_back(IndexEntry::deserialize(index_data.data(), offset));
    }

    // 读取布隆过滤器
    fseek(file_, footer_.bloom_filter_offset, SEEK_SET);
    std::vector<char> bloom_data(footer_.bloom_filter_size);
    if (fread(bloom_data.data(), 1, footer_.bloom_filter_size, file_) != footer_.bloom_filter_size)
    {
        LOG_ERROR("Failed to read bloom filter from SSTable: {}", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    size_t bloom_offset = 0;
    bloom_filter_ = BloomFilter::deserialize(bloom_data.data(), bloom_offset);

    // 填充元数据
    meta_.filepath = filepath_;
    meta_.min_key = min_key;
    meta_.max_key = max_key;
    meta_.file_size = file_size;
    meta_.entry_count = footer_.entry_count;
    meta_.level = level;

    // 从文件名提取序列号
    std::filesystem::path path(filepath_);
    std::string filename = path.stem().string();
    try
    {
        meta_.sequence_number = std::stoull(filename);
    }
    catch (...)
    {
        meta_.sequence_number = 0;
    }

    LOG_INFO("Loaded SSTable: {} with {} entries", filepath_.c_str(), footer_.entry_count);

    return true;
}

bool SSTable::may_contain(const std::string &key) const
{
    // 先检查范围
    if (!meta_.may_contain_key(key))
    {
        return false;
    }
    // 再检查布隆过滤器
    return bloom_filter_.may_contain(key);
}

bool SSTable::may_contain(std::string_view key) const
{
    // 先检查范围
    if (!meta_.may_contain_key(key))
    {
        return false;
    }
    // 再检查布隆过滤器
    return bloom_filter_.may_contain(key);
}

size_t SSTable::find_block_index(const std::string &key) const
{
    if (index_.empty())
        return 0;

    // 二分查找找到最后一个 first_key <= key 的数据块
    size_t left = 0;
    size_t right = index_.size();

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        if (index_[mid].first_key <= key)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    return left > 0 ? left - 1 : 0;
}

size_t SSTable::find_block_index(std::string_view key) const
{
    if (index_.empty())
        return 0;

    // 二分查找找到最后一个 first_key <= key 的数据块
    size_t left = 0;
    size_t right = index_.size();

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        if (index_[mid].first_key <= key)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    return left > 0 ? left - 1 : 0;
}

BlockDataPtr SSTable::read_data_block(size_t block_index) const
{
    auto entries = std::make_shared<BlockData>();
    if (block_index >= index_.size())
        return entries;

    // 1. 构造唯一的 Cache Key (序列号_块索引)
    std::string cache_key;
    if (block_cache_)
    {
        cache_key = std::to_string(meta_.sequence_number) + "_" + std::to_string(block_index);
        if (block_cache_->get(cache_key, entries))
            return entries; // 缓存命中！直接返回，0 I/O 开销
    }

    const auto &idx = index_[block_index];
    std::vector<char> block_data(idx.block_size);

    // 2. 缓存未命中，原子读取磁盘
    if (!pread_exact(file_, block_data.data(), idx.block_size, idx.block_offset))
    {
        LOG_ERROR("Failed to read data block from SSTable: {}", filepath_.c_str());
        return entries;
    }

    // 3. 反序列化
    size_t offset = 0;
    while (offset < idx.block_size)
    {
        if (offset + sizeof(uint32_t) > idx.block_size)
            break;
        uint32_t key_len;
        std::memcpy(&key_len, block_data.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);

        if (offset + key_len > idx.block_size)
            break;
        std::string key(block_data.data() + offset, key_len);
        offset += key_len;

        if (offset + sizeof(uint32_t) > idx.block_size)
            break;
        uint32_t value_len;
        std::memcpy(&value_len, block_data.data() + offset, sizeof(value_len));
        offset += sizeof(value_len);

        if (offset + value_len > idx.block_size)
            break;

        size_t value_offset = 0;
        EValue value = deserialize(block_data.data() + offset, value_offset);
        offset += value_len;

        entries->emplace_back(std::move(key), std::move(value));
    }

    // 4. 写入缓存
    if (block_cache_)
    {
        block_cache_->put(cache_key, entries);
    }

    return entries;
}

std::optional<EValue> SSTable::search_in_block(
    const std::vector<std::pair<std::string, EValue>> &block,
    const std::string &key) const
{

    // 二分查找
    auto it = std::lower_bound(block.begin(), block.end(), key,
                               [](const auto &pair, const std::string &k)
                               {
                                   return pair.first < k;
                               });

    if (it != block.end() && it->first == key)
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<EValue> SSTable::search_in_block(
    const std::vector<std::pair<std::string, EValue>> &block,
    std::string_view key) const
{

    // 二分查找
    auto it = std::lower_bound(block.begin(), block.end(), key,
                               [](const auto &pair, const auto &k)
                               {
                                   return pair.first < k;
                               });

    if (it != block.end() && it->first == key)
    {
        return it->second;
    }
    return std::nullopt;
}
std::optional<EValue> SSTable::get(const std::string &key) const
{
    // 使用布隆过滤器快速排除
    if (!may_contain(key))
    {
        return std::nullopt;
    }

    // 找到可能包含 key 的数据块
    size_t block_index = find_block_index(key);

    // 读取并搜索数据块
    auto block = read_data_block(block_index);
    auto result = search_in_block(*block, key);

    return result;
}

std::optional<EValue> SSTable::get(std::string_view key) const
{
    // 使用布隆过滤器快速排除
    if (!may_contain(key))
    {
        return std::nullopt;
    }

    // 找到可能包含 key 的数据块
    size_t block_index = find_block_index(key);

    // 读取并搜索数据块
    auto block = read_data_block(block_index);
    auto result = search_in_block(*block, key);

    return result;
}

std::vector<std::pair<std::string, EValue>> SSTable::range(
    const std::string &start_key,
    const std::string &end_key) const
{

    std::vector<std::pair<std::string, EValue>> result;

    // 检查范围是否与 SSTable 有交集
    if (start_key > meta_.max_key || end_key < meta_.min_key)
    {
        return result;
    }

    // 找到起始数据块
    size_t start_block = find_block_index(start_key);

    // 遍历可能的数据块
    for (size_t i = start_block; i < index_.size(); ++i)
    {
        // 如果数据块的起始 key 已经超过 end_key，停止
        if (i > start_block && index_[i].first_key > end_key)
        {
            break;
        }

        auto block = read_data_block(i);
        for (const auto &[k, v] : *block)
        {
            if (k >= start_key && k <= end_key)
            {
                result.emplace_back(k, v);
            }
            else if (k > end_key)
            {
                break;
            }
        }
    }

    return result;
}

std::map<std::string, EValue> SSTable::range_map(
    const std::string &start_key,
    const std::string &end_key) const
{

    std::map<std::string, EValue> result;

    // 检查范围是否与 SSTable 有交集
    if (start_key > meta_.max_key || end_key < meta_.min_key)
    {
        return result;
    }

    // 找到起始数据块
    size_t start_block = find_block_index(start_key);

    // 遍历可能的数据块
    for (size_t i = start_block; i < index_.size(); ++i)
    {
        // 如果数据块的起始 key 已经超过 end_key，停止
        if (i > start_block && index_[i].first_key > end_key)
        {
            break;
        }

        auto block = read_data_block(i);
        for (const auto &[k, v] : *block)
        {
            if (k >= start_key && k <= end_key)
            {
                result[k] = v;
            }
            else if (k > end_key)
            {
                break;
            }
        }
    }

    return result;
}

void SSTable::for_each(const std::function<bool(const std::string &, const EValue &)> &callback) const
{
    for (size_t i = 0; i < index_.size(); ++i)
    {
        auto block = read_data_block(i);
        for (const auto &[k, v] : *block)
        {
            if (!callback(k, v))
            {
                return;
            }
        }
    }
}

// SSTableBuilder 实现
SSTableBuilder::SSTableBuilder(const std::string &filepath, const uint32_t level, size_t block_size)
    : filepath_(filepath), level_(level), block_size_(block_size), entry_count_(0), current_offset_(0), entries_in_block_(0), finished_(false), aborted_(false), file_(nullptr)
{
    file_ = fopen(filepath_.c_str(), "wb");
    if (file_ == nullptr)
    {
        LOG_ERROR("Cannot create SSTable file: {}", filepath_.c_str());
        aborted_ = true;
    }
}

SSTableBuilder::~SSTableBuilder()
{
    if (!finished_ && !aborted_)
    {
        abort();
    }
    if (file_ != nullptr)
    {
        fclose(file_);
        file_ = nullptr;
    }
}

std::string SSTableBuilder::serialize_entry(const std::string &key, const EValue &value)
{
    std::string result;
    // 序列化 value
    std::string value_data = serialize(value);
    uint32_t value_len = static_cast<uint32_t>(value_data.size());
    // 空间预分配
    result.reserve(sizeof(uint32_t) * 2 + key.size() + value_data.size());
    // 写入 key 长度和内容
    uint32_t key_len = static_cast<uint32_t>(key.size());
    result.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
    result.append(key);
    result.append(reinterpret_cast<const char *>(&value_len), sizeof(value_len));
    result.append(value_data);
    return result;
}

void SSTableBuilder::add(const std::string &key, const EValue &value)
{
    if (finished_ || aborted_)
    {
        LOG_ERROR("Cannot add to finished or aborted SSTableBuilder");
        return;
    }

    // 初始化布隆过滤器（延迟初始化，预估10000个条目）
    if (!bloom_filter_)
    {
        bloom_filter_ = std::make_unique<BloomFilter>(10000);
    }

    // 添加到布隆过滤器
    bloom_filter_->add(key);

    // 记录 min/max key
    if (entry_count_ == 0)
    {
        min_key_ = key;
    }
    max_key_ = key;

    // 序列化 KV 对
    std::string entry_data = serialize_entry(key, value);

    // 记录数据块的第一个 key
    if (entries_in_block_ == 0)
    {
        first_key_in_block_ = key;
    }

    // 添加到当前数据块
    current_block_.append(entry_data);
    entries_in_block_++;
    entry_count_++;

    // 如果数据块已满，刷新到文件
    if (current_block_.size() >= block_size_)
    {
        flush_block();
    }
}

void SSTableBuilder::flush_block()
{
    if (current_block_.empty())
    {
        return;
    }

    // 记录索引条目
    IndexEntry entry;
    entry.first_key = first_key_in_block_;
    entry.block_offset = current_offset_;
    entry.block_size = current_block_.size();
    index_entries_.push_back(entry);

    // 写入数据块
    fwrite(current_block_.data(), 1, current_block_.size(), file_);
    current_offset_ += current_block_.size();

    // 清空当前数据块
    current_block_.clear();
    first_key_in_block_.clear();
    entries_in_block_ = 0;
}

void SSTableBuilder::write_footer()
{
    SSTableFooter footer;

    // 索引块信息（已在 finish 中计算）
    size_t index_start = current_offset_;
    std::string index_data;
    for (const auto &entry : index_entries_)
    {
        index_data.append(entry.serialize());
    }

    footer.index_block_offset = index_start;
    footer.index_block_size = index_data.size();

    // 写入索引块
    fwrite(index_data.data(), 1, index_data.size(), file_);
    current_offset_ += index_data.size();

    // 布隆过滤器信息
    size_t bloom_start = current_offset_;
    std::string bloom_data;
    if (bloom_filter_)
    {
        bloom_data = bloom_filter_->serialize();
        fwrite(bloom_data.data(), 1, bloom_data.size(), file_);
        current_offset_ += bloom_data.size();
    }

    footer.bloom_filter_offset = bloom_start;
    footer.bloom_filter_size = bloom_data.size();

    // 写入 min_key 和 max_key
    footer.min_key_offset = current_offset_;
    footer.min_key_size = min_key_.size();
    fwrite(min_key_.data(), 1, min_key_.size(), file_);
    current_offset_ += min_key_.size();

    footer.max_key_offset = current_offset_;
    footer.max_key_size = max_key_.size();
    fwrite(max_key_.data(), 1, max_key_.size(), file_);
    current_offset_ += max_key_.size();

    // 其他元数据
    footer.data_block_count = index_entries_.size();
    footer.entry_count = entry_count_;
    footer.magic = SSTABLE_MAGIC_NUMBER;

    // 写入 Footer
    std::string footer_data = footer.serialize();
    fwrite(footer_data.data(), 1, footer_data.size(), file_);
    current_offset_ += footer_data.size();
    // 写入level
    fwrite(&level_, 1, sizeof(level_), file_);
    current_offset_ += sizeof(level_);
}

bool SSTableBuilder::finish()
{
    if (finished_ || aborted_)
    {
        return false;
    }

    // 刷新最后一个数据块
    flush_block();

    // 写入 Footer（包含索引块和布隆过滤器）
    write_footer();
    fflush(file_);
#ifdef _WIN32
    int fd = _fileno(file_);
    if (fd == -1)
    {
        LOG_ERROR("SSTableBuilder: Failed to get file descriptor for syncing.");
        return false;
    }
    if (_commit(fd) != 0)
    {
        LOG_ERROR("SSTableBuilder: Failed to sync SSTable file to disk.");
        return false;
    }
#else
    // 步骤1：获取底层文件描述符
    int fd = fileno(file_); // 从FILE*获取fd
    if (fd == -1)
    {
        LOG_ERROR("SSTableBuilder: Failed to get file descriptor for syncing.");
        return false;
    }

    // 步骤2：调用fsync刷内核缓冲区到磁盘（真正落盘）
    if (fdatasync(fd) == -1)
    { // fdatasync(fd) 更高效（仅刷数据）
        LOG_ERROR("SSTableBuilder: Failed to sync SSTable file to disk. Error: {}", strerror(errno));
        return false;
    }
#endif
    // TODO 刷到磁盘中
    file_ = nullptr;
    finished_ = true;

    LOG_INFO("SSTable created: {} with {} entries", filepath_.c_str(), entry_count_);

    return true;
}

void SSTableBuilder::abort()
{
    if (finished_ || aborted_)
    {
        return;
    }

    if (file_ != nullptr)
    {
        fclose(file_);
        file_ = nullptr;
    }
    aborted_ = true;

    // 删除临时文件
    try
    {
        std::filesystem::remove(filepath_);
    }
    catch (...)
    {
        LOG_WARN("Failed to remove aborted SSTable file: {}", filepath_.c_str());
    }
}

// SSTableManager 实现
SSTableManager::SSTableManager(const std::string &data_dir,
                               const SSTableMergeStrategy &merge_strategy,
                               const uint32_t &sstable_merge_threshold,
                               const uint64_t &sstable_zero_level_size,
                               const double &sstable_level_size_ratio)
    : data_dir_(data_dir), merge_strategy_(merge_strategy), sstable_merge_threshold_(sstable_merge_threshold),
      sstable_zero_level_size_(sstable_zero_level_size * 1024 * 1024), sstable_level_size_ratio_(sstable_level_size_ratio),
      next_sequence_number_(1), sstable_count_(0), max_level_(0)
{
    // 确保目录存在
    if (!std::filesystem::exists(data_dir_))
    {
        std::filesystem::create_directories(data_dir_);
    }
    block_cache_ = std::make_shared<BlockCache>(BlockCacheCapacity);
    // 加载现有的 SSTable 文件
    load_all();
}

std::string SSTableManager::generate_filename()
{
    // 使用序列号生成文件名
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(16) << next_sequence_number_++;
    return PathUtils::combine_path(data_dir_, oss.str() + ".sst");
}

bool SSTableManager::load_all()
{
    level_sstables_.clear();
    level_sstable_size_.clear();
    level_mutex_.clear();
    sstable_count_ = 0;
    level_sstables_.resize(max_level_ + 1);
    level_sstable_size_.resize(max_level_ + 1, 0);
    level_mutex_.resize(max_level_ + 1);
    for (size_t i = 0; i <= max_level_; ++i)
    {
        level_mutex_[i] = std::make_unique<std::shared_mutex>();
    }
    next_sequence_number_ = 1;

    if (!std::filesystem::exists(data_dir_))
    {
        return true;
    }

    for (const auto &entry : std::filesystem::directory_iterator(data_dir_))
    {
        auto filepath = entry.path().string();
        auto ext = entry.path().extension().string();
        LOG_INFO("SSTableManager::load_all: Found file: {}, extension: '{}'", filepath.c_str(), ext.c_str());

        if (entry.path().extension() == ".sst")
        {
            try
            {
                LOG_INFO("SSTableManager::load_all: Loading SSTable: {}", filepath.c_str());
                auto sstable = std::make_shared<SSTable>(entry.path().string(), block_cache_);

                // 更新下一个序列号
                uint64_t seq = sstable->get_meta().sequence_number;
                if (seq >= next_sequence_number_)
                {
                    next_sequence_number_ = seq + 1;
                }
                uint32_t level = sstable->get_meta().level;
                if (level > max_level_)
                {
                    size_t old_max = max_level_;
                    max_level_ = level;
                    level_sstables_.resize(max_level_ + 1);
                    level_sstable_size_.resize(max_level_ + 1, 0);
                    level_mutex_.resize(max_level_ + 1);
                    for (size_t i = old_max + 1; i <= max_level_; ++i)
                    {
                        level_mutex_[i] = std::make_unique<std::shared_mutex>();
                    }
                }
                // 在 std::move 之前保存 file_size，避免使用已移动对象
                uint64_t file_size = sstable->get_meta().file_size;
                level_sstables_[level].push_back(std::move(sstable));
                level_sstable_size_[level] += file_size;
                sstable_count_++;
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Failed to load SSTable: {}, error: {}",
                          entry.path().string().c_str(), e.what());
            }
        }
    }

    // 按序列号排序（最新的在前）
    sort_sstables_by_sequence();

    normalize_sstables();
    LOG_INFO("Loaded {} SSTable files from {}, max level: {}, level_sstables_.size(): {}", sstable_count_, data_dir_.c_str(), max_level_, level_sstables_.size());
    return true;
}

void SSTableManager::normalize_sstables()
{
    // 读取数据目录下面的.smeta文件
    std::string smeta_file = PathUtils::combine_path(data_dir_, ".smeta");
    if (!std::filesystem::exists(smeta_file))
    {
        LOG_INFO("SSTableManager: .smeta file not found, merge all sstables to max level");
        for (int i = 0; i < max_level_; i++)
        {
            merge_sstables_by_strategy_0(i);
        }
        FILE *file = fopen(smeta_file.c_str(), "wb");
        if (file == nullptr)
        {
            LOG_ERROR("Failed to open .smeta file: {} to write merge strategy", smeta_file.c_str());
            return;
        }
        std::string strategy_str = std::to_string(static_cast<int>(merge_strategy_));
        fwrite(strategy_str.data(), 1, strategy_str.size(), file);
        fclose(file);
    }
    else
    {
        FILE *file = fopen(smeta_file.c_str(), "rb");
        if (file == nullptr)
        {
            LOG_ERROR("Failed to open .smeta file: {}, merge all sstables to max level", smeta_file.c_str());
            for (int i = 0; i < max_level_; i++)
            {
                merge_sstables_by_strategy_0(i);
            }
            return;
        }
        // 读取文件内容
        std::string content;
        fseek(file, 0, SEEK_END);
        content.resize(ftell(file));
        rewind(file);
        if (fread(content.data(), 1, content.size(), file) != content.size())
        {
            LOG_ERROR("Failed to read .smeta file: {}", smeta_file.c_str());
            fclose(file);
            for (int i = 0; i < max_level_; i++)
            {
                merge_sstables_by_strategy_0(i);
            }
            return;
        }
        fclose(file);
        SSTableMergeStrategy last_strategy;
        try
        {
            last_strategy = static_cast<SSTableMergeStrategy>(std::stoi(content));
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to parse .smeta file: {}, merge all sstables to max level", smeta_file.c_str());
            for (int i = 0; i < max_level_; i++)
            {
                merge_sstables_by_strategy_0(i);
            }
        }
        if (last_strategy != merge_strategy_)
        {
            LOG_INFO("SSTableManager: merge strategy changed, merge all sstables to max level");
            for (int i = 0; i < max_level_; i++)
            {
                merge_sstables_by_strategy_0(i);
            }
            file = fopen(smeta_file.c_str(), "wb");
            if (file == nullptr)
            {
                LOG_ERROR("Failed to open .smeta file: {} to write merge strategy", smeta_file.c_str());
                return;
            }
            std::string strategy_str = std::to_string(static_cast<int>(merge_strategy_));
            fwrite(strategy_str.data(), 1, strategy_str.size(), file);
            fclose(file);
        }
    }
}

bool SSTableManager::merge_sstables(const uint32_t level)
{
    // 获取全局状态时轻量锁
    uint32_t curr_max_level;
    {
        std::shared_lock<std::shared_mutex> lock(manager_mutex_);
        curr_max_level = max_level_;
    }
    if (level > curr_max_level)
        return false;
    if (merge_strategy_ == SSTableMergeStrategy::SIZE_TIERED_COMPACTION)
    {
        bool should_merge = false;
        {
            std::shared_lock<std::shared_mutex> lock(*level_mutex_[level]);
            should_merge = level_sstables_[level].size() >= sstable_merge_threshold_;
        }
        return should_merge ? merge_sstables_by_strategy_0(level) : true;
    }
    else if (merge_strategy_ == SSTableMergeStrategy::LEVEL_COMPACTION)
    {
        bool should_merge = false;
        {
            std::shared_lock<std::shared_mutex> lock(*level_mutex_[level]);
            should_merge = level_sstable_size_[level] >= sstable_zero_level_size_ * pow(sstable_level_size_ratio_, level);
        }
        return should_merge ? merge_sstables_by_strategy_1(level) : true;
    }
    else
    {
        throw std::runtime_error("Unknown merge strategy");
    }
}

bool SSTableManager::merge_sstables_by_strategy_0(const uint32_t level)
{
    // [性能优化] 无锁合并核心：先抓取文件指针的 shared_ptr 副本，立即释放该层的锁
    std::vector<std::shared_ptr<SSTable>> sstables_to_merge;
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[level]);
        if (level_sstables_[level].empty())
            return true;
        sstables_to_merge = level_sstables_[level]; // 安全拷贝
    }

    std::map<std::string, EValue> map;
    for (auto it = sstables_to_merge.rbegin(); it != sstables_to_merge.rend(); ++it)
    {
        (*it)->for_each([&map](const std::string &key, const EValue &value)
                        {
            map[key] = value;
            return true; });
    }

    std::vector<std::pair<std::string, EValue>> entries;
    for (const auto &[key, value] : map)
    {
        if (!value.is_deleted() && !value.is_expired())
            entries.emplace_back(key, value);
    }

    if (!entries.empty())
    {
        auto meta = create_from_entries(entries, level + 1);
        if (meta == std::nullopt)
            return false;
    }

    // 文件新下沉合并完毕，快速上写锁移除废弃索引
    {
        std::unique_lock<std::shared_mutex> lock(*level_mutex_[level]);
        auto &sstables = level_sstables_[level];
        // 移除刚才我们提取的那些老文件
        for (const auto &merged_sst : sstables_to_merge)
        {
            auto it = std::find(sstables.begin(), sstables.end(), merged_sst);
            if (it != sstables.end())
            {
                level_sstable_size_[level] -= (*it)->get_meta().file_size;
                sstables.erase(it);
            }
        }
    }

    // 从磁盘物理干掉旧文件
    for (const auto &sst : sstables_to_merge)
    {
        std::filesystem::remove(sst->get_meta().filepath);
    }

    merge_sstables(level + 1);
    return true;
}

bool SSTableManager::merge_sstables_by_strategy_1(const uint32_t level)
{
    uint32_t curr_max_level;
    {
        std::shared_lock<std::shared_mutex> lock(manager_mutex_);
        curr_max_level = max_level_;
    }

    if (level == curr_max_level)
        return merge_sstables_by_strategy_0(level);

    std::shared_ptr<SSTable> sst_to_merge;
    std::vector<std::shared_ptr<SSTable>> next_sstables_to_merge;

    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[level]);
        std::shared_lock<std::shared_mutex> lock2(*level_mutex_[level + 1]);
        if (level_sstables_[level].empty())
            return true;

        // 抓取该层最旧的一个进行下沉
        sst_to_merge = level_sstables_[level].back();

        // 提取下层存在范围重叠的目标文件
        for (const auto &next_sst : level_sstables_[level + 1])
        {
            if (!(sst_to_merge->get_meta().max_key < next_sst->get_meta().min_key ||
                  sst_to_merge->get_meta().min_key > next_sst->get_meta().max_key))
            {
                next_sstables_to_merge.push_back(next_sst);
            }
        }
    }

    // 无锁进行内存归并和物理写入
    std::map<std::string, EValue> map;
    for (auto it = next_sstables_to_merge.rbegin(); it != next_sstables_to_merge.rend(); ++it)
    {
        (*it)->for_each([&map](const std::string &key, const EValue &value)
                        {
            map[key] = value; return true; });
    }
    sst_to_merge->for_each([&map](const std::string &key, const EValue &value)
                           {
        map[key] = value; return true; });

    std::vector<std::pair<std::string, EValue>> entries;
    for (const auto &[key, value] : map)
    {
        if (!value.is_deleted() && !value.is_expired())
            entries.emplace_back(key, value);
    }

    if (!entries.empty())
    {
        auto meta = create_from_entries(entries, level + 1);
        if (meta == std::nullopt)
            return false;
    }

    // 合并落盘完毕，极速短锁剔除老节点
    {
        std::unique_lock<std::shared_mutex> lock2(*level_mutex_[level + 1]);
        for (const auto &old_sst : next_sstables_to_merge)
        {
            auto it = std::find(level_sstables_[level + 1].begin(), level_sstables_[level + 1].end(), old_sst);
            if (it != level_sstables_[level + 1].end())
            {
                level_sstable_size_[level + 1] -= (*it)->get_meta().file_size;
                level_sstables_[level + 1].erase(it);
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(*level_mutex_[level]);
        auto it = std::find(level_sstables_[level].begin(), level_sstables_[level].end(), sst_to_merge);
        if (it != level_sstables_[level].end())
        {
            level_sstable_size_[level] -= (*it)->get_meta().file_size;
            level_sstables_[level].erase(it);
        }
    }

    std::filesystem::remove(sst_to_merge->get_meta().filepath);
    for (const auto &sst : next_sstables_to_merge)
    {
        std::filesystem::remove(sst->get_meta().filepath);
    }

    merge_sstables(level);
    merge_sstables(level + 1);
    return true;
}

void SSTableManager::sort_sstables_by_sequence()
{
    std::unique_lock<std::shared_mutex> global_lock(manager_mutex_);
    for (uint32_t i = 0; i <= max_level_; ++i)
    {
        std::unique_lock<std::shared_mutex> lock(*level_mutex_[i]);
        auto &sstables = level_sstables_[i];
        std::sort(sstables.begin(), sstables.end(), [](const auto &a, const auto &b)
                  { return a->get_meta().sequence_number > b->get_meta().sequence_number; });
    }
}

std::optional<SSTableMeta> SSTableManager::create_from_entries(
    const std::vector<std::pair<std::string, EValue>> &entries, const uint32_t level)
{

    if (entries.empty())
    {
        return std::nullopt;
    }

    std::string filepath = generate_filename();
    SSTableBuilder builder(filepath, level);

    for (const auto &[key, value] : entries)
    {
        builder.add(key, value);
    }

    if (!builder.finish())
    {
        LOG_ERROR("Failed to create SSTable: {}", filepath.c_str());
        return std::nullopt;
    }

    // 加载新创建的 SSTable
    auto sstable = std::make_shared<SSTable>(filepath);
    SSTableMeta meta = sstable->get_meta();

    // 动态扩容层级
    {
        std::unique_lock<std::shared_mutex> global_lock(manager_mutex_);
        if (level > max_level_)
        {
            size_t old_max = max_level_;
            max_level_ = level;
            level_sstables_.resize(max_level_ + 1);
            level_sstable_size_.resize(max_level_ + 1, 0);
            for (size_t i = old_max + 1; i <= max_level_; ++i)
            {
                level_mutex_.push_back(std::make_unique<std::shared_mutex>());
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(*level_mutex_[level]);
        level_sstables_[level].insert(level_sstables_[level].begin(), sstable);
        level_sstable_size_[level] += meta.file_size;
    }
    return meta;
}

bool SSTableManager::get(const std::string &key, EValue *value) const
{
    uint32_t curr_max;
    {
        std::shared_lock<std::shared_mutex> global_lock(manager_mutex_);
        curr_max = max_level_;
    }
    // 按顺序查询（最新的在前）
    LOG_INFO("SSTableManager::get key={},level_sstables_.size()={}", key.c_str(), level_sstables_.size());
    for (uint32_t i = 0; i <= curr_max; ++i)
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[i]);
        LOG_INFO("SSTableManager::get level={}, sstables.size()={}", i, level_sstables_[i].size());
        for (const auto &sstable : level_sstables_[i])
        {
            auto result = sstable->get(key);
            if (result.has_value())
            {
                if (value)
                {
                    *value = result.value();
                    LOG_INFO("SSTableManager::get found key={}, value={}", key.c_str(), to_string(value->value).c_str());
                }
                return true;
            }
        }
    }
    return false;
}
bool SSTableManager::get(std::string_view key, EValue *value) const
{
    uint32_t curr_max;
    {
        std::shared_lock<std::shared_mutex> global_lock(manager_mutex_);
        curr_max = max_level_;
    }
    // 按顺序查询（最新的在前）
    LOG_INFO("SSTableManager::get key={},level_sstables_.size()={}", key, level_sstables_.size());
    for (uint32_t i = 0; i <= curr_max; ++i)
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[i]);
        LOG_INFO("SSTableManager::get level={}, sstables.size()={}", i, level_sstables_[i].size());
        for (const auto &sstable : level_sstables_[i])
        {
            // TODO 优化
            auto result = sstable->get(key);
            if (result.has_value())
            {
                if (value)
                {
                    *value = result.value();
                    LOG_INFO("SSTableManager::get found key={}, value={}", key, to_string(value->value).c_str());
                }
                return true;
            }
        }
    }
    return false;
}
size_t SSTableManager::get_total_size() const
{
    size_t total = 0;
    std::shared_lock<std::shared_mutex> global_lock(manager_mutex_);
    for (uint32_t i = 0; i <= max_level_; ++i)
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[i]);
        total += level_sstable_size_[i];
    }
    return total;
}

std::optional<SSTableMeta> SSTableManager::create_new_sstable(const std::vector<std::pair<std::string, EValue>> &entries)
{
    std::optional<SSTableMeta> meta = create_from_entries(entries, 0);
    if (meta.has_value())
    {
        merge_sstables(0);
    }
    return meta;
}

std::map<std::string, EValue> SSTableManager::range_query(
    const std::string &start_key,
    const std::string &end_key) const
{
    std::map<std::string, EValue> map;
    for_each_oldest([&](const std::string &key, const EValue &value)
                    {
        if (key >= start_key && key <= end_key) map[key] = value;
        return true; });
    return map;
}

std::map<std::string, EValue> SSTableManager::range_query(
    std::string_view start_key,
    std::string_view end_key) const
{
    std::map<std::string, EValue> map;
    for_each_oldest([&](const std::string &key, const EValue &value)
                    {
        if (key >= start_key && key <= end_key) map[key] = value;
        return true; });
    return map;
}

void SSTableManager::for_each_newest(std::function<bool(const std::string &key, const EValue &value)> callback) const
{
    uint32_t curr_max;
    {
        std::shared_lock<std::shared_mutex> global_lock(manager_mutex_);
        curr_max = max_level_;
    }

    // 提前拿到 shared_ptr 副本释放全局大锁
    std::vector<std::shared_ptr<SSTable>> sst_copy;
    for (uint32_t i = 0; i <= curr_max; ++i)
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[i]);
        for (const auto &sst : level_sstables_[i])
            sst_copy.push_back(sst);
    }
    for (const auto &sst : sst_copy)
        sst->for_each(callback);
}

void SSTableManager::for_each_oldest(std::function<bool(const std::string &key, const EValue &value)> callback) const
{
    uint32_t curr_max;
    {
        std::shared_lock<std::shared_mutex> global_lock(manager_mutex_);
        curr_max = max_level_;
    }

    // 提前一次性把所有的指针抓出来，使得长耗时的大批量迭代期间无任何锁竞争
    std::vector<std::shared_ptr<SSTable>> sst_copy;
    for (int i = curr_max; i >= 0; --i)
    {
        std::shared_lock<std::shared_mutex> lock(*level_mutex_[i]);
        for (auto sit = level_sstables_[i].rbegin(); sit != level_sstables_[i].rend(); ++sit)
        {
            sst_copy.push_back(*sit);
        }
    }
    for (const auto &sst : sst_copy)
        sst->for_each(callback);
}