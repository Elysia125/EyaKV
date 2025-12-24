#include "storage/sstable.h"
#include "logger/logger.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>

// ==================== SSTableFooter 实现 ====================

std::string SSTableFooter::serialize() const {
    std::string result;
    result.reserve(SIZE);
    
    auto append_uint64 = [&result](uint64_t val) {
        result.append(reinterpret_cast<const char*>(&val), sizeof(val));
    };
    auto append_uint32 = [&result](uint32_t val) {
        result.append(reinterpret_cast<const char*>(&val), sizeof(val));
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
    append_uint32(magic);
    
    return result;
}

SSTableFooter SSTableFooter::deserialize(const char* data) {
    SSTableFooter footer;
    size_t offset = 0;
    
    auto read_uint64 = [&data, &offset]() -> uint64_t {
        uint64_t val;
        std::memcpy(&val, data + offset, sizeof(val));
        offset += sizeof(val);
        return val;
    };
    auto read_uint32 = [&data, &offset]() -> uint32_t {
        uint32_t val;
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
    footer.magic = read_uint32();
    
    return footer;
}

// ==================== IndexEntry 实现 ====================

std::string IndexEntry::serialize() const {
    std::string result;
    
    // 写入 first_key 长度和内容
    uint32_t key_len = static_cast<uint32_t>(first_key.size());
    result.append(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
    result.append(first_key);
    
    // 写入 block_offset 和 block_size
    result.append(reinterpret_cast<const char*>(&block_offset), sizeof(block_offset));
    result.append(reinterpret_cast<const char*>(&block_size), sizeof(block_size));
    
    return result;
}

IndexEntry IndexEntry::deserialize(const char* data, size_t& offset) {
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

// ==================== BloomFilter 实现 ====================

BloomFilter::BloomFilter() : num_hash_functions_(0) {}

BloomFilter::BloomFilter(size_t expected_elements, size_t bits_per_key) 
    : num_hash_functions_(std::min(static_cast<size_t>(30), 
                                   std::max(static_cast<size_t>(1), 
                                            static_cast<size_t>(bits_per_key * 0.69)))) // ln(2) ≈ 0.69
{
    size_t total_bits = expected_elements * bits_per_key;
    size_t num_bytes = (total_bits + 7) / 8;
    bits_.resize(num_bytes, 0);
}

std::vector<uint32_t> BloomFilter::getHashes(const std::string& key) const {
    std::vector<uint32_t> hashes;
    hashes.reserve(num_hash_functions_);
    
    // 使用 MurmurHash3 的简化版本计算两个基础哈希
    uint32_t h1 = 0;
    uint32_t h2 = 0;
    
    for (size_t i = 0; i < key.size(); ++i) {
        h1 = h1 * 31 + static_cast<uint8_t>(key[i]);
        h2 = h2 * 37 + static_cast<uint8_t>(key[i]);
    }
    
    // 使用双哈希技术生成多个哈希值
    for (size_t i = 0; i < num_hash_functions_; ++i) {
        hashes.push_back(h1 + i * h2);
    }
    
    return hashes;
}

void BloomFilter::add(const std::string& key) {
    if (bits_.empty()) return;
    
    size_t num_bits = bits_.size() * 8;
    auto hashes = getHashes(key);
    
    for (uint32_t hash : hashes) {
        size_t bit_pos = hash % num_bits;
        bits_[bit_pos / 8] |= (1 << (bit_pos % 8));
    }
}

bool BloomFilter::mayContain(const std::string& key) const {
    if (bits_.empty()) return true;
    
    size_t num_bits = bits_.size() * 8;
    auto hashes = getHashes(key);
    
    for (uint32_t hash : hashes) {
        size_t bit_pos = hash % num_bits;
        if (!(bits_[bit_pos / 8] & (1 << (bit_pos % 8)))) {
            return false;
        }
    }
    return true;
}

std::string BloomFilter::serialize() const {
    std::string result;
    
    // 写入哈希函数数量
    uint32_t num_hashes = static_cast<uint32_t>(num_hash_functions_);
    result.append(reinterpret_cast<const char*>(&num_hashes), sizeof(num_hashes));
    
    // 写入位数组大小和内容
    uint32_t bits_size = static_cast<uint32_t>(bits_.size());
    result.append(reinterpret_cast<const char*>(&bits_size), sizeof(bits_size));
    result.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
    
    return result;
}

BloomFilter BloomFilter::deserialize(const char* data, size_t size) {
    BloomFilter filter;
    size_t offset = 0;
    
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

// ==================== SSTable 实现 ====================

SSTable::SSTable(const std::string& filepath) : filepath_(filepath) {
    if (!Load()) {
        LOG_ERROR("Failed to load SSTable: " + filepath);
    }
}

SSTable::~SSTable() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool SSTable::Load() {
    file_.open(filepath_, std::ios::binary);
    if (!file_.is_open()) {
        LOG_ERROR("Cannot open SSTable file: " + filepath_);
        return false;
    }
    
    // 获取文件大小
    file_.seekg(0, std::ios::end);
    size_t file_size = file_.tellg();
    
    if (file_size < SSTableFooter::SIZE) {
        LOG_ERROR("SSTable file too small: " + filepath_);
        return false;
    }
    
    // 读取 Footer
    file_.seekg(file_size - SSTableFooter::SIZE, std::ios::beg);
    std::vector<char> footer_data(SSTableFooter::SIZE);
    file_.read(footer_data.data(), SSTableFooter::SIZE);
    footer_ = SSTableFooter::deserialize(footer_data.data());
    
    // 验证魔数
    if (footer_.magic != SSTABLE_MAGIC_NUMBER) {
        LOG_ERROR("Invalid SSTable magic number: " + filepath_);
        return false;
    }
    
    // 读取 min_key 和 max_key
    std::string min_key, max_key;
    if (footer_.min_key_size > 0) {
        file_.seekg(footer_.min_key_offset, std::ios::beg);
        min_key.resize(footer_.min_key_size);
        file_.read(&min_key[0], footer_.min_key_size);
    }
    if (footer_.max_key_size > 0) {
        file_.seekg(footer_.max_key_offset, std::ios::beg);
        max_key.resize(footer_.max_key_size);
        file_.read(&max_key[0], footer_.max_key_size);
    }
    
    // 读取索引块
    file_.seekg(footer_.index_block_offset, std::ios::beg);
    std::vector<char> index_data(footer_.index_block_size);
    file_.read(index_data.data(), footer_.index_block_size);
    
    size_t offset = 0;
    while (offset < footer_.index_block_size) {
        index_.push_back(IndexEntry::deserialize(index_data.data(), offset));
    }
    
    // 读取布隆过滤器
    file_.seekg(footer_.bloom_filter_offset, std::ios::beg);
    std::vector<char> bloom_data(footer_.bloom_filter_size);
    file_.read(bloom_data.data(), footer_.bloom_filter_size);
    bloom_filter_ = BloomFilter::deserialize(bloom_data.data(), footer_.bloom_filter_size);
    
    // 填充元数据
    meta_.filepath = filepath_;
    meta_.min_key = min_key;
    meta_.max_key = max_key;
    meta_.file_size = file_size;
    meta_.entry_count = footer_.entry_count;
    meta_.level = 0;  // 默认为 Level 0
    
    // 从文件名提取序列号
    std::filesystem::path path(filepath_);
    std::string filename = path.stem().string();
    try {
        meta_.sequence_number = std::stoull(filename);
    } catch (...) {
        meta_.sequence_number = 0;
    }
    
    LOG_INFO("Loaded SSTable: " + filepath_ + " with " + 
             std::to_string(footer_.entry_count) + " entries");
    
    return true;
}

bool SSTable::MayContain(const std::string& key) const {
    // 先检查范围
    if (!meta_.mayContainKey(key)) {
        return false;
    }
    // 再检查布隆过滤器
    return bloom_filter_.mayContain(key);
}

size_t SSTable::FindBlockIndex(const std::string& key) const {
    if (index_.empty()) return 0;
    
    // 二分查找找到最后一个 first_key <= key 的数据块
    size_t left = 0;
    size_t right = index_.size();
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (index_[mid].first_key <= key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    return left > 0 ? left - 1 : 0;
}

std::vector<std::pair<std::string, EyaValue>> SSTable::ReadDataBlock(size_t block_index) const {
    std::vector<std::pair<std::string, EyaValue>> entries;
    
    if (block_index >= index_.size()) {
        return entries;
    }
    
    const auto& idx = index_[block_index];
    
    // 读取数据块
    file_.seekg(idx.block_offset, std::ios::beg);
    std::vector<char> block_data(idx.block_size);
    file_.read(block_data.data(), idx.block_size);
    
    // 解析数据块中的 KV 对
    size_t offset = 0;
    while (offset < idx.block_size) {
        // 读取 key 长度和内容
        if (offset + sizeof(uint32_t) > idx.block_size) break;
        uint32_t key_len;
        std::memcpy(&key_len, block_data.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);
        
        if (offset + key_len > idx.block_size) break;
        std::string key(block_data.data() + offset, key_len);
        offset += key_len;
        
        // 读取 value 长度和内容
        if (offset + sizeof(uint32_t) > idx.block_size) break;
        uint32_t value_len;
        std::memcpy(&value_len, block_data.data() + offset, sizeof(value_len));
        offset += sizeof(value_len);
        
        if (offset + value_len > idx.block_size) break;
        
        // 反序列化 EyaValue
        size_t value_offset = 0;
        EyaValue value = deserialize_eya_value(block_data.data() + offset, value_offset);
        offset += value_len;
        
        entries.emplace_back(std::move(key), std::move(value));
    }
    
    return entries;
}

std::optional<EyaValue> SSTable::SearchInBlock(
    const std::vector<std::pair<std::string, EyaValue>>& block,
    const std::string& key) const {
    
    // 二分查找
    auto it = std::lower_bound(block.begin(), block.end(), key,
        [](const auto& pair, const std::string& k) {
            return pair.first < k;
        });
    
    if (it != block.end() && it->first == key) {
        return it->second;
    }
    return std::nullopt;
}

bool SSTable::Get(const std::string& key, EyaValue* value) const {
    // 使用布隆过滤器快速排除
    if (!MayContain(key)) {
        return false;
    }
    
    // 找到可能包含 key 的数据块
    size_t block_index = FindBlockIndex(key);
    
    // 读取并搜索数据块
    auto block = ReadDataBlock(block_index);
    auto result = SearchInBlock(block, key);
    
    if (result.has_value()) {
        if (value) {
            *value = result.value();
        }
        return true;
    }
    
    return false;
}

std::vector<std::pair<std::string, EyaValue>> SSTable::Range(
    const std::string& start_key, 
    const std::string& end_key) const {
    
    std::vector<std::pair<std::string, EyaValue>> result;
    
    // 检查范围是否与 SSTable 有交集
    if (start_key > meta_.max_key || end_key < meta_.min_key) {
        return result;
    }
    
    // 找到起始数据块
    size_t start_block = FindBlockIndex(start_key);
    
    // 遍历可能的数据块
    for (size_t i = start_block; i < index_.size(); ++i) {
        // 如果数据块的起始 key 已经超过 end_key，停止
        if (i > start_block && index_[i].first_key > end_key) {
            break;
        }
        
        auto block = ReadDataBlock(i);
        for (const auto& [k, v] : block) {
            if (k >= start_key && k <= end_key) {
                result.emplace_back(k, v);
            } else if (k > end_key) {
                break;
            }
        }
    }
    
    return result;
}

void SSTable::ForEach(const std::function<bool(const std::string&, const EyaValue&)>& callback) const {
    for (size_t i = 0; i < index_.size(); ++i) {
        auto block = ReadDataBlock(i);
        for (const auto& [k, v] : block) {
            if (!callback(k, v)) {
                return;
            }
        }
    }
}

// ==================== SSTableBuilder 实现 ====================

SSTableBuilder::SSTableBuilder(const std::string& filepath, size_t block_size)
    : filepath_(filepath)
    , block_size_(block_size)
    , entry_count_(0)
    , current_offset_(0)
    , entries_in_block_(0)
    , finished_(false)
    , aborted_(false)
{
    file_.open(filepath_, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        LOG_ERROR("Cannot create SSTable file: " + filepath_);
        aborted_ = true;
    }
}

SSTableBuilder::~SSTableBuilder() {
    if (!finished_ && !aborted_) {
        Abort();
    }
    if (file_.is_open()) {
        file_.close();
    }
}

std::string SSTableBuilder::SerializeEntry(const std::string& key, const EyaValue& value) {
    std::string result;
    
    // 写入 key 长度和内容
    uint32_t key_len = static_cast<uint32_t>(key.size());
    result.append(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
    result.append(key);
    
    // 序列化 value
    std::string value_data = serialize_eya_value(value);
    uint32_t value_len = static_cast<uint32_t>(value_data.size());
    result.append(reinterpret_cast<const char*>(&value_len), sizeof(value_len));
    result.append(value_data);
    
    return result;
}

void SSTableBuilder::Add(const std::string& key, const EyaValue& value) {
    if (finished_ || aborted_) {
        LOG_ERROR("Cannot add to finished or aborted SSTableBuilder");
        return;
    }
    
    // 初始化布隆过滤器（延迟初始化，预估10000个条目）
    if (!bloom_filter_) {
        bloom_filter_ = std::make_unique<BloomFilter>(10000);
    }
    
    // 添加到布隆过滤器
    bloom_filter_->add(key);
    
    // 记录 min/max key
    if (entry_count_ == 0) {
        min_key_ = key;
    }
    max_key_ = key;
    
    // 序列化 KV 对
    std::string entry_data = SerializeEntry(key, value);
    
    // 记录数据块的第一个 key
    if (entries_in_block_ == 0) {
        first_key_in_block_ = key;
    }
    
    // 添加到当前数据块
    current_block_.append(entry_data);
    entries_in_block_++;
    entry_count_++;
    
    // 如果数据块已满，刷新到文件
    if (current_block_.size() >= block_size_) {
        FlushBlock();
    }
}

void SSTableBuilder::FlushBlock() {
    if (current_block_.empty()) {
        return;
    }
    
    // 记录索引条目
    IndexEntry entry;
    entry.first_key = first_key_in_block_;
    entry.block_offset = current_offset_;
    entry.block_size = current_block_.size();
    index_entries_.push_back(entry);
    
    // 写入数据块
    file_.write(current_block_.data(), current_block_.size());
    current_offset_ += current_block_.size();
    
    // 清空当前数据块
    current_block_.clear();
    first_key_in_block_.clear();
    entries_in_block_ = 0;
}

void SSTableBuilder::WriteIndexBlock() {
    std::string index_data;
    for (const auto& entry : index_entries_) {
        index_data.append(entry.serialize());
    }
    file_.write(index_data.data(), index_data.size());
}

void SSTableBuilder::WriteBloomFilter() {
    if (bloom_filter_) {
        std::string bloom_data = bloom_filter_->serialize();
        file_.write(bloom_data.data(), bloom_data.size());
    }
}

void SSTableBuilder::WriteFooter() {
    SSTableFooter footer;
    
    // 索引块信息（已在 Finish 中计算）
    size_t index_start = current_offset_;
    std::string index_data;
    for (const auto& entry : index_entries_) {
        index_data.append(entry.serialize());
    }
    
    footer.index_block_offset = index_start;
    footer.index_block_size = index_data.size();
    
    // 写入索引块
    file_.write(index_data.data(), index_data.size());
    current_offset_ += index_data.size();
    
    // 布隆过滤器信息
    size_t bloom_start = current_offset_;
    std::string bloom_data;
    if (bloom_filter_) {
        bloom_data = bloom_filter_->serialize();
        file_.write(bloom_data.data(), bloom_data.size());
        current_offset_ += bloom_data.size();
    }
    
    footer.bloom_filter_offset = bloom_start;
    footer.bloom_filter_size = bloom_data.size();
    
    // 写入 min_key 和 max_key
    footer.min_key_offset = current_offset_;
    footer.min_key_size = min_key_.size();
    file_.write(min_key_.data(), min_key_.size());
    current_offset_ += min_key_.size();
    
    footer.max_key_offset = current_offset_;
    footer.max_key_size = max_key_.size();
    file_.write(max_key_.data(), max_key_.size());
    current_offset_ += max_key_.size();
    
    // 其他元数据
    footer.data_block_count = index_entries_.size();
    footer.entry_count = entry_count_;
    footer.magic = SSTABLE_MAGIC_NUMBER;
    
    // 写入 Footer
    std::string footer_data = footer.serialize();
    file_.write(footer_data.data(), footer_data.size());
}

bool SSTableBuilder::Finish() {
    if (finished_ || aborted_) {
        return false;
    }
    
    // 刷新最后一个数据块
    FlushBlock();
    
    // 写入 Footer（包含索引块和布隆过滤器）
    WriteFooter();
    
    file_.flush();
    file_.close();
    finished_ = true;
    
    LOG_INFO("SSTable created: " + filepath_ + " with " + 
             std::to_string(entry_count_) + " entries");
    
    return true;
}

SSTableMeta SSTableBuilder::GetMeta() const {
    SSTableMeta meta;
    meta.filepath = filepath_;
    meta.min_key = min_key_;
    meta.max_key = max_key_;
    meta.file_size = current_offset_;
    meta.entry_count = entry_count_;
    meta.level = 0;
    meta.sequence_number = 0;
    return meta;
}

void SSTableBuilder::Abort() {
    if (finished_ || aborted_) {
        return;
    }
    
    file_.close();
    aborted_ = true;
    
    // 删除临时文件
    try {
        std::filesystem::remove(filepath_);
    } catch (...) {
        LOG_WARN("Failed to remove aborted SSTable file: " + filepath_);
    }
}

// ==================== SSTableManager 实现 ====================

SSTableManager::SSTableManager(const std::string& data_dir)
    : data_dir_(data_dir)
    , next_sequence_number_(1)
{
    // 确保目录存在
    if (!std::filesystem::exists(data_dir_)) {
        std::filesystem::create_directories(data_dir_);
    }
    
    // 加载现有的 SSTable 文件
    LoadAll();
}

std::string SSTableManager::GenerateFilename() {
    // 使用序列号生成文件名
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(16) << next_sequence_number_++;
    return data_dir_ + "/" + oss.str() + ".sst";
}

bool SSTableManager::LoadAll() {
    sstables_.clear();
    next_sequence_number_ = 1;
    
    if (!std::filesystem::exists(data_dir_)) {
        return true;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (entry.path().extension() == ".sst") {
            try {
                auto sstable = std::make_unique<SSTable>(entry.path().string());
                
                // 更新下一个序列号
                uint64_t seq = sstable->GetMeta().sequence_number;
                if (seq >= next_sequence_number_) {
                    next_sequence_number_ = seq + 1;
                }
                
                sstables_.push_back(std::move(sstable));
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to load SSTable: " + entry.path().string() + 
                         ", error: " + e.what());
            }
        }
    }
    
    // 按序列号排序（最新的在前）
    SortSSTablesBySequence();
    
    LOG_INFO("Loaded " + std::to_string(sstables_.size()) + " SSTable files from " + data_dir_);
    
    return true;
}

void SSTableManager::SortSSTablesBySequence() {
    std::sort(sstables_.begin(), sstables_.end(),
        [](const auto& a, const auto& b) {
            return a->GetMeta().sequence_number > b->GetMeta().sequence_number;
        });
}

std::optional<SSTableMeta> SSTableManager::CreateFromEntries(
    const std::vector<std::pair<std::string, EyaValue>>& entries) {
    
    if (entries.empty()) {
        return std::nullopt;
    }
    
    std::string filepath = GenerateFilename();
    SSTableBuilder builder(filepath);
    
    for (const auto& [key, value] : entries) {
        builder.Add(key, value);
    }
    
    if (!builder.Finish()) {
        LOG_ERROR("Failed to create SSTable: " + filepath);
        return std::nullopt;
    }
    
    // 加载新创建的 SSTable
    auto sstable = std::make_unique<SSTable>(filepath);
    SSTableMeta meta = sstable->GetMeta();
    
    // 添加到管理列表
    sstables_.insert(sstables_.begin(), std::move(sstable));
    
    return meta;
}

bool SSTableManager::Get(const std::string& key, EyaValue* value) const {
    // 按顺序查询（最新的在前）
    for (const auto& sstable : sstables_) {
        if (sstable->Get(key, value)) {
            return true;
        }
    }
    return false;
}

size_t SSTableManager::GetTotalSize() const {
    size_t total = 0;
    for (const auto& sstable : sstables_) {
        total += sstable->GetMeta().file_size;
    }
    return total;
}
