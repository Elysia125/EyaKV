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
#include <io.h>
#else
#include <unistd.h> // 包含fsync/fdatasync（Linux/macOS）
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
SSTable::SSTable(const std::string &filepath) : filepath_(filepath)
{
    if (!load())
    {
        LOG_ERROR("Failed to load SSTable: %s", filepath.c_str());
    }
}

SSTable::~SSTable()
{
    if (file_ != nullptr)
    {
        LOG_INFO("SSTable::~SSTable: Closing file %s", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        LOG_INFO("SSTable::~SSTable: File %s closed", filepath_.c_str());
    }
}

bool SSTable::load()
{
    file_ = fopen(filepath_.c_str(), "rb");
    if (file_ == nullptr)
    {
        LOG_ERROR("Cannot open SSTable file: %s", filepath_.c_str());
        return false;
    }

    // 获取文件大小
    fseek(file_, 0, SEEK_END);
    size_t file_size = ftell(file_);

    if (file_size < SSTableFooter::SIZE)
    {
        LOG_ERROR("SSTable file too small: %s", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    // 读取level
    uint32_t level;
    fseek(file_, file_size - sizeof(level), SEEK_SET);
    if (fread(&level, 1, sizeof(level), file_) != sizeof(level))
    {
        LOG_ERROR("Failed to read level from SSTable: %s", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    // 读取 Footer
    fseek(file_, file_size - sizeof(level) - SSTableFooter::SIZE, SEEK_SET);
    std::vector<char> footer_data(SSTableFooter::SIZE);
    if (fread(footer_data.data(), 1, SSTableFooter::SIZE, file_) != SSTableFooter::SIZE)
    {
        LOG_ERROR("Failed to read footer from SSTable: %s", filepath_.c_str());
        fclose(file_);
        file_ = nullptr;
        return false;
    }
    footer_ = SSTableFooter::deserialize(footer_data.data());

    // 验证魔数
    if (footer_.magic != SSTABLE_MAGIC_NUMBER)
    {
        LOG_ERROR("Invalid SSTable magic number: %s", filepath_.c_str());
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
            LOG_ERROR("Failed to read min_key from SSTable: %s", filepath_.c_str());
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
            LOG_ERROR("Failed to read max_key from SSTable: %s", filepath_.c_str());
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
        LOG_ERROR("Failed to read index block from SSTable: %s", filepath_.c_str());
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
        LOG_ERROR("Failed to read bloom filter from SSTable: %s", filepath_.c_str());
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

    LOG_INFO("Loaded SSTable: %s with %zu entries", filepath_.c_str(), footer_.entry_count);

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

std::vector<std::pair<std::string, EValue>> SSTable::read_data_block(size_t block_index) const
{
    std::vector<std::pair<std::string, EValue>> entries;

    if (block_index >= index_.size())
    {
        return entries;
    }

    const auto &idx = index_[block_index];

    // 读取数据块
    fseek(file_, idx.block_offset, SEEK_SET);
    std::vector<char> block_data(idx.block_size);
    if (fread(block_data.data(), 1, idx.block_size, file_) != idx.block_size)
    {
        LOG_ERROR("Failed to read data block from SSTable: %s", filepath_.c_str());
        return entries;
    }

    // 解析数据块中的 KV 对
    size_t offset = 0;
    while (offset < idx.block_size)
    {
        // 读取 key 长度和内容
        if (offset + sizeof(uint32_t) > idx.block_size)
            break;
        uint32_t key_len;
        std::memcpy(&key_len, block_data.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);

        if (offset + key_len > idx.block_size)
            break;
        std::string key(block_data.data() + offset, key_len);
        offset += key_len;

        // 读取 value 长度和内容
        if (offset + sizeof(uint32_t) > idx.block_size)
            break;
        uint32_t value_len;
        std::memcpy(&value_len, block_data.data() + offset, sizeof(value_len));
        offset += sizeof(value_len);

        if (offset + value_len > idx.block_size)
            break;

        // 反序列化 EValue
        size_t value_offset = 0;
        EValue value = deserialize(block_data.data() + offset, value_offset);
        offset += value_len;

        entries.emplace_back(std::move(key), std::move(value));
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
    auto result = search_in_block(block, key);

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
        for (const auto &[k, v] : block)
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
        for (const auto &[k, v] : block)
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
        for (const auto &[k, v] : block)
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
        LOG_ERROR("Cannot create SSTable file: %s", filepath_.c_str());
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

    // 写入 key 长度和内容
    uint32_t key_len = static_cast<uint32_t>(key.size());
    result.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
    result.append(key);

    // 序列化 value
    std::string value_data = serialize(value);
    uint32_t value_len = static_cast<uint32_t>(value_data.size());
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
        LOG_ERROR("SSTableBuilder: Failed to sync SSTable file to disk. Error: %s", strerror(errno));
        return false;
    }
#endif
    // TODO 刷到磁盘中
    file_ = nullptr;
    finished_ = true;

    LOG_INFO("SSTable created: %s with %zu entries", filepath_.c_str(), entry_count_);

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
        LOG_WARN("Failed to remove aborted SSTable file: %s", filepath_.c_str());
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
        level_mutex_[i] = std::make_unique<std::recursive_mutex>();
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
        LOG_INFO("SSTableManager::load_all: Found file: %s, extension: '%s'", filepath.c_str(), ext.c_str());

        if (entry.path().extension() == ".sst")
        {
            try
            {
                LOG_INFO("SSTableManager::load_all: Loading SSTable: %s", filepath.c_str());
                auto sstable = std::make_unique<SSTable>(entry.path().string());

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
                        level_mutex_[i] = std::make_unique<std::recursive_mutex>();
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
                LOG_ERROR("Failed to load SSTable: %s, error: %s",
                          entry.path().string().c_str(), e.what());
            }
        }
    }

    // 按序列号排序（最新的在前）
    sort_sstables_by_sequence();

    normalize_sstables();
    LOG_INFO("Loaded %zu SSTable files from %s, max level: %d, level_sstables_.size(): %zu", sstable_count_, data_dir_.c_str(), max_level_, level_sstables_.size());
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
            LOG_ERROR("Failed to open .smeta file: %s to write merge strategy", smeta_file.c_str());
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
            LOG_ERROR("Failed to open .smeta file: %s, merge all sstables to max level", smeta_file.c_str());
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
            LOG_ERROR("Failed to read .smeta file: %s", smeta_file.c_str());
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
            LOG_ERROR("Failed to parse .smeta file: %s, merge all sstables to max level", smeta_file.c_str());
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
                LOG_ERROR("Failed to open .smeta file: %s to write merge strategy", smeta_file.c_str());
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
    if (level > max_level_)
    {
        return false;
    }
    if (merge_strategy_ == SSTableMergeStrategy::SIZE_TIERED_COMPACTION)
    {
        if (level_sstables_[level].size() < sstable_merge_threshold_)
        {
            return true;
        }
        return merge_sstables_by_strategy_0(level);
    }
    else if (merge_strategy_ == SSTableMergeStrategy::LEVEL_COMPACTION)
    {
        if (level_sstable_size_[level] < sstable_zero_level_size_ * pow(sstable_level_size_ratio_, level))
        {
            return true;
        }
        return merge_sstables_by_strategy_1(level);
    }
    else
    {
        throw std::runtime_error("Unknown merge strategy");
    }
}

bool SSTableManager::merge_sstables_by_strategy_0(const uint32_t level)
{
    {
        std::lock_guard<std::recursive_mutex> lock(*level_mutex_[level]);
        std::vector<std::unique_ptr<SSTable>> &sstables = level_sstables_[level];
        if (sstables.size() < 1)
        {
            return true;
        }
        std::map<std::string, EValue> map;
        std::vector<std::string> file_paths;
        for (auto it = sstables.rbegin(); it != sstables.rend(); it++)
        {
            (*it)->for_each([&map](const std::string &key, const EValue &value)
                            {
            map[key] = value;
            return true; });
            file_paths.push_back((*it)->get_meta().filepath);
        }
        std::vector<std::pair<std::string, EValue>> entries;
        for (const auto &[key, value] : map)
        {
            if (value.is_deleted() || value.is_expired())
            {
                continue;
            }
            entries.emplace_back(key, value);
        }
        if (entries.empty())
        {
            // 如果没有有效的数据，则删除所有sstable文件
            for (const auto &file_path : file_paths)
            {
                std::filesystem::remove(file_path);
            }
            level_sstables_[level].clear();
            level_sstable_size_[level] = 0;
            return true;
        }
        auto meta = create_from_entries(entries, level + 1);
        if (meta == std::nullopt)
        {
            LOG_ERROR("Failed to merge SSTable files of level %d", level);
            return false;
        }
        // 删除旧的sstable文件
        for (const auto &file_path : file_paths)
        {
            std::filesystem::remove(file_path);
        }
        level_sstables_[level].clear();
        level_sstable_size_[level] = 0;
    }
    merge_sstables(level + 1);
    return true;
}

bool SSTableManager::merge_sstables_by_strategy_1(const uint32_t level)
{
    if (level == max_level_ || level_sstables_[level + 1].empty())
    {
        // 最后一层，直接合并
        if (level_sstables_[level].size() > 1)
        {
            return merge_sstables_by_strategy_0(level);
        }
        else
        {
            // 如果只有一个sstable文件，则不合并
            return true;
        }
    }
    else
    {
        bool merged = false;
        {
            std::lock_guard<std::recursive_mutex> lock(*level_mutex_[level]);
            std::lock_guard<std::recursive_mutex> lock2(*level_mutex_[level + 1]);
            // 选择level层的sstable文件中最旧的一个文件
            auto &sstables = level_sstables_[level];
            if (sstables.empty())
            {
                return true;
            }
            // 获取level+1层的所有sstables
            auto &next_sstables = level_sstables_[level + 1];
            int rindex = 0;
            for (auto it = sstables.rbegin(); it != sstables.rend(); it++)
            {
                rindex++;
                auto &sstable = *it;
                std::vector<uint32_t> merged_sstables_index;
                std::vector<std::string> file_paths;
                file_paths.push_back(sstable->get_filepath());
                for (int i = 0; i < next_sstables.size(); i++)
                {
                    auto &next_sstable = next_sstables[i];
                    // 如果有重叠则加入合并
                    if (!(sstable->get_meta().max_key < next_sstable->get_meta().min_key ||
                          sstable->get_meta().min_key > next_sstable->get_meta().max_key))
                    {
                        merged_sstables_index.push_back(i);
                        file_paths.push_back(next_sstable->get_filepath());
                    }
                }
                if (merged_sstables_index.empty())
                {
                    continue;
                }
                std::map<std::string, EValue> map;
                for (auto mit = merged_sstables_index.rbegin(); mit != merged_sstables_index.rend(); mit++)
                {
                    auto &sst = next_sstables[*mit];
                    sst->for_each([&map](const std::string &key, const EValue &value)
                                  {
            map[key] = value;
            return true; });
                }
                sstable->for_each([&map](const std::string &key, const EValue &value)
                                  {
            map[key] = value;
            return true; });
                std::vector<std::pair<std::string, EValue>> entries;
                for (const auto &[key, value] : map)
                {
                    if (value.is_deleted() || value.is_expired())
                    {
                        continue;
                    }
                    entries.emplace_back(key, value);
                }
                if (entries.empty())
                {
                    // 如果合并的sstable文件中没有有效的数据
                    // 删除旧的sstable文件
                    for (const auto &file_path : file_paths)
                    {
                        std::filesystem::remove(file_path);
                    }
                    for (auto mit = merged_sstables_index.rbegin(); mit != merged_sstables_index.rend(); mit++)
                    {
                        level_sstable_size_[level + 1] -= next_sstables[*mit]->get_meta().file_size;
                        next_sstables.erase(next_sstables.begin() + *mit);
                    }
                    sstables.erase(sstables.end() - rindex);
                    level_sstable_size_[level] -= sstable->get_meta().file_size;
                    merged = true;
                    break;
                }
                auto meta = create_from_entries(entries, level + 1);
                if (meta == std::nullopt)
                {
                    LOG_ERROR("Failed to merge SSTable files of level %d", level);
                    return false;
                }
                // 删除旧的sstable文件
                for (const auto &file_path : file_paths)
                {
                    std::filesystem::remove(file_path);
                }
                for (auto mit = merged_sstables_index.rbegin(); mit != merged_sstables_index.rend(); mit++)
                {
                    level_sstable_size_[level + 1] -= next_sstables[*mit]->get_meta().file_size;
                    next_sstables.erase(next_sstables.begin() + *mit);
                }
                sstables.erase(sstables.end() - rindex);
                level_sstable_size_[level] -= sstable->get_meta().file_size;
                merged = true;
                break;
            }
        }
        if (merged)
        {
            // 合并完成后，继续合并下一层的sstable文件
            merge_sstables(level); // 确保该层大小符合要求
            merge_sstables(level + 1);
            return true;
        }
        else
        {
            // 没有合并，说明level层的sstable文件全部文件都与level+1层的sstable文件不重叠
            // 直接合并该层的所有sstable文件
            return merge_sstables_by_strategy_0(level);
        }
    }
}

void SSTableManager::sort_sstables_by_sequence()
{
    for (auto &sstables : level_sstables_)
    {
        std::sort(sstables.begin(), sstables.end(),
                  [](const auto &a, const auto &b)
                  {
                      return a->get_meta().sequence_number > b->get_meta().sequence_number;
                  });
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
        LOG_ERROR("Failed to create SSTable: %s", filepath.c_str());
        return std::nullopt;
    }

    // 加载新创建的 SSTable
    auto sstable = std::make_unique<SSTable>(filepath);
    SSTableMeta meta = sstable->get_meta();

    // 添加到管理列表
    if (level > max_level_)
    {
        size_t old_max = max_level_;
        max_level_ = level;
        level_sstables_.resize(max_level_ + 1);
        level_sstable_size_.resize(max_level_ + 1, 0);
        level_mutex_.resize(max_level_ + 1);
        for (size_t i = old_max + 1; i <= max_level_; ++i)
        {
            level_mutex_[i] = std::make_unique<std::recursive_mutex>();
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lock(*level_mutex_[level]);
        level_sstables_[level].insert(level_sstables_[level].begin(), std::move(sstable));
        level_sstable_size_[level] += meta.file_size;
    }
    return meta;
}

bool SSTableManager::get(const std::string &key, EValue *value) const
{
    // 按顺序查询（最新的在前）
    LOG_INFO("SSTableManager::get key=%s,level_sstables_.size()=%d", key.c_str(), level_sstables_.size());
    int current_level = 0;
    for (const auto &sstables : level_sstables_)
    {
        LOG_INFO("SSTableManager::get level=%d, sstables.size()=%d", current_level, sstables.size());
        for (const auto &sstable : sstables)
        {
            auto result = sstable->get(key);
            if (result.has_value())
            {
                if (value)
                {
                    *value = result.value();
                    LOG_INFO("SSTableManager::get found key=%s, value=%s", key.c_str(), to_string(value->value).c_str());
                }
                return true;
            }
        }
        current_level++;
    }
    return false;
}

size_t SSTableManager::get_total_size() const
{
    size_t total = 0;
    for (const auto level_size : level_sstable_size_)
    {
        total += level_size;
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
    for (auto it = level_sstables_.rbegin(); it != level_sstables_.rend(); it++)
    {
        for (auto sit = it->rbegin(); sit != it->rend(); sit++)
        {
            std::map<std::string, EValue> entries = (*sit)->range_map(start_key, end_key);
            map.insert(entries.begin(), entries.end());
        }
    }
    return map;
}

void SSTableManager::for_each_newest(std::function<bool(const std::string &key, const EValue &value)> callback) const
{
    for (auto it = level_sstables_.begin(); it != level_sstables_.end(); it++)
    {
        for (auto sit = it->begin(); sit != it->end(); sit++)
        {
            (*sit)->for_each(callback);
        }
    }
}

void SSTableManager::for_each_oldest(std::function<bool(const std::string &key, const EValue &value)> callback) const
{
    for (auto it = level_sstables_.rbegin(); it != level_sstables_.rend(); it++)
    {
        for (auto sit = it->rbegin(); sit != it->rend(); sit++)
        {
            (*sit)->for_each(callback);
        }
    }
}