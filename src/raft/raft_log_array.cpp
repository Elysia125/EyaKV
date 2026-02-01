#include "raft/raft.h"
#include "logger/logger.h"
#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include "common/util/path_utils.h"
#include "common/util/file_utils.h"
#include "common/util/checksum_utils.h"
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#define LOG_SIZE_THRESHOLD 1000000 // 100万条目

// 日志文件格式 (模仿 leveldb log format):
// [Checksum (4 bytes)] [Length (4 bytes)] [Data (N bytes)]
// Checksum = CRC32 of [Length][Data]

// RaftLogArray 实现
RaftLogArray::RaftLogArray(const std::string &log_dir)
    : entries_(), base_index_(1), log_dir_(log_dir)
{
    // 确保 log_dir 存在
    if (!create_directory(log_dir_))
    {
        LOG_ERROR("Failed to create log directory: %s", log_dir_.c_str());
        throw std::runtime_error("Failed to create log directory: " + log_dir_);
    }
    // 打开 WAL 文件 (append 模式)
    wal_path_ = PathUtils::combine_path(log_dir_, "raft_wal.log");
    wal_file_ = fopen(wal_path_.c_str(), "ab+");
    if (wal_file_ == nullptr)
    {
        LOG_ERROR("Failed to open WAL file: %s", wal_path_.c_str());
        throw std::runtime_error("Failed to open WAL file: " + wal_path_);
    }

    // 打开索引文件
    index_path_ = PathUtils::combine_path(log_dir_, "raft_index.idx");
    index_file_ = fopen(index_path_.c_str(), "ab+");
    if (index_file_ == nullptr)
    {
        LOG_ERROR("Failed to open index file: %s", index_path_.c_str());
        throw std::runtime_error("Failed to open index file: " + index_path_);
    }
    // 尝试恢复
    recover();
}

RaftLogArray::~RaftLogArray()
{
    // 刷盘并关闭文件
    sync_metadata();

    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }

    if (index_file_ != nullptr)
    {
        fclose(index_file_);
        index_file_ = nullptr;
    }
}

uint32_t RaftLogArray::compute_checksum(const std::string &data) const
{
    ChecksumCalculator cc;
    return cc.fromString(data);
}

bool RaftLogArray::write_entry_to_wal(const LogEntry &entry, uint64_t &offset)
{
    if (wal_file_ == nullptr)
    {
        return false;
    }

    // 记录写入位置
    offset = ftell(wal_file_);

    // 序列化日志条目
    std::string data = entry.serialize();
    uint32_t length = static_cast<uint32_t>(data.size());
    uint32_t checksum = compute_checksum(data);

    // 写入: [Checksum (4)] [Length (4)] [Data (N)]
    if (fwrite(&checksum, sizeof(uint32_t), 1, wal_file_) != 1)
    {
        LOG_ERROR("Failed to write checksum to WAL");
        return false;
    }

    if (fwrite(&length, sizeof(uint32_t), 1, wal_file_) != 1)
    {
        LOG_ERROR("Failed to write length to WAL");
        return false;
    }

    if (fwrite(data.data(), 1, length, wal_file_) != length)
    {
        LOG_ERROR("Failed to write data to WAL");
        return false;
    }

    fflush(wal_file_);
    return true;
}

bool RaftLogArray::write_batch_to_wal(const std::vector<LogEntry> &entries)
{
    if (wal_file_ == nullptr || entries.empty())
    {
        return false;
    }

    // 写入每个条目
    for (const auto &entry : entries)
    {
        std::string data = entry.serialize();
        uint32_t length = static_cast<uint32_t>(data.size());
        uint32_t checksum = compute_checksum(data);

        if (fwrite(&checksum, sizeof(uint32_t), 1, wal_file_) != 1 ||
            fwrite(&length, sizeof(uint32_t), 1, wal_file_) != 1 ||
            fwrite(data.data(), 1, length, wal_file_) != length)
        {
            LOG_ERROR("Failed to write batch entry to WAL");
            return false;
        }
    }

    fflush(wal_file_);
    return true;
}

bool RaftLogArray::read_entry_from_wal(uint64_t offset, LogEntry &entry) const
{
    if (wal_file_ == nullptr)
    {
        return false;
    }

    // 保存当前文件位置
    long current_pos = ftell(wal_file_);

    // 定位到指定偏移
    if (fseek(wal_file_, static_cast<long>(offset), SEEK_SET) != 0)
    {
        LOG_ERROR("Failed to seek to offset %lu in WAL", offset);
        return false;
    }

    // 读取: [Checksum (4)] [Length (4)] [Data (N)]
    uint32_t checksum_read, length;
    if (fread(&checksum_read, sizeof(uint32_t), 1, wal_file_) != 1 ||
        fread(&length, sizeof(uint32_t), 1, wal_file_) != 1)
    {
        LOG_ERROR("Failed to read entry header from WAL");
        fseek(wal_file_, current_pos, SEEK_SET); // 恢复位置
        return false;
    }

    // 读取数据
    std::string data(length, '\0');
    if (fread(&data[0], 1, length, wal_file_) != length)
    {
        LOG_ERROR("Failed to read entry data from WAL");
        fseek(wal_file_, current_pos, SEEK_SET); // 恢复位置
        return false;
    }

    // 验证校验和
    uint32_t checksum_calc = compute_checksum(data);
    if (checksum_read != checksum_calc)
    {
        LOG_ERROR("Checksum mismatch in WAL entry");
        fseek(wal_file_, current_pos, SEEK_SET); // 恢复位置
        return false;
    }

    // 反序列化
    size_t off = 0;
    entry = LogEntry::deserialize(data.c_str(), off);
    fseek(wal_file_, current_pos, SEEK_SET); // 恢复位置
    return false;
}

bool RaftLogArray::write_index_entry(uint64_t offset)
{
    if (index_file_ == nullptr)
    {
        return false;
    }

    // 追加索引项: [Offset (8)]
    if (fwrite(&offset, sizeof(uint64_t), 1, index_file_) != 1)
    {
        LOG_ERROR("Failed to write index entry");
        return false;
    }

    fflush(index_file_);
    return true;
}

bool RaftLogArray::append_to_index(uint64_t offset)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    index_offsets_.push_back(offset);
    return write_index_entry(offset);
}

bool RaftLogArray::append(LogEntry &entry)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entry.index = base_index_ + entries_.size();

    // 1. 写入 WAL
    uint64_t offset;
    if (!write_entry_to_wal(entry, offset))
    {
        LOG_ERROR("Failed to write entry to WAL");
        return -1;
    }

    // 2. 追加到内存
    entries_.push_back(entry);

    // 3. 更新索引
    index_offsets_.push_back(offset);
    if (!write_index_entry(offset))
    {
        LOG_WARN("Failed to write index entry to file");
    }
    if (entries_.size() > LOG_SIZE_THRESHOLD)
    {
        LOG_WARN("Raft log size exceeded threshold, truncating.");
        truncate_from(entries_.size() / 4); // 截断前四分之一日志
    }
    return true;
}

bool RaftLogArray::append(const LogEntry &entry)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // 1. 写入 WAL
    uint64_t offset;
    if (!write_entry_to_wal(entry, offset))
    {
        LOG_ERROR("Failed to write entry to WAL");
        return -1;
    }

    // 2. 追加到内存
    entries_.push_back(entry);

    // 3. 更新索引
    index_offsets_.push_back(offset);
    if (!write_index_entry(offset))
    {
        LOG_WARN("Failed to write index entry to file");
    }
    if (entries_.size() > LOG_SIZE_THRESHOLD)
    {
        LOG_WARN("Raft log size exceeded threshold, truncating.");
        truncate_from(entries_.size() / 4); // 截断前四分之一日志
    }
    return true;
}

bool RaftLogArray::batch_append(std::vector<LogEntry> &entries)
{
    if (entries.empty())
    {
        return true;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    uint32_t start_index = base_index_ + entries_.size();
    for (auto &entry : entries)
    {
        entry.index = start_index++;
    }
    // 1. 写入 WAL (批量)
    uint64_t offset = ftell(wal_file_);
    if (!write_batch_to_wal(entries))
    {
        LOG_ERROR("Failed to write batch to WAL");
        return false;
    }

    // 2. 追加到内存
    for (const auto &entry : entries)
    {
        entries_.push_back(entry);
        index_offsets_.push_back(offset);
        offset += sizeof(uint32_t) * 2 + entry.serialize().size(); // 估算下一个偏移
    }
    if (entries_.size() > LOG_SIZE_THRESHOLD)
    {
        LOG_WARN("Raft log size exceeded threshold, truncating.");
        truncate_from(entries_.size() / 4); // 截断前四分之一日志
    }
    return true;
}

bool RaftLogArray::batch_append(const std::vector<LogEntry> &entries)
{
    if (entries.empty())
    {
        return true;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // 1. 写入 WAL (批量)
    uint64_t offset = ftell(wal_file_);
    if (!write_batch_to_wal(entries))
    {
        LOG_ERROR("Failed to write batch to WAL");
        return false;
    }

    // 2. 追加到内存
    for (const auto &entry : entries)
    {
        entries_.push_back(entry);
        index_offsets_.push_back(offset);
        offset += sizeof(uint32_t) * 2 + entry.serialize().size(); // 估算下一个偏移
    }
    if (entries_.size() > LOG_SIZE_THRESHOLD)
    {
        LOG_WARN("Raft log size exceeded threshold, truncating.");
        truncate_from(entries_.size() / 4); // 截断前四分之一日志
    }
    return true;
}

bool RaftLogArray::get(uint32_t index, LogEntry &entry) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (index < base_index_ || index >= base_index_ + entries_.size())
    {
        return false;
    }

    size_t array_index = index - base_index_;
    entry = entries_[array_index];
    return true;
}

bool RaftLogArray::get_last(LogEntry &entry) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (entries_.empty())
    {
        return false;
    }

    entry = entries_.back();
    return true;
}

bool RaftLogArray::get_last_info(uint32_t &index, uint32_t &term) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (entries_.empty())
    {
        index = 0;
        term = 0;
        return false;
    }

    index = base_index_ + entries_.size() - 1;
    term = entries_.back().term;
    return true;
}

uint32_t RaftLogArray::size() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return static_cast<uint32_t>(entries_.size());
}

uint32_t RaftLogArray::get_last_index() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (entries_.empty())
    {
        return 0;
    }
    return base_index_ + entries_.size() - 1;
}

uint32_t RaftLogArray::get_base_index() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return base_index_;
}

std::vector<LogEntry> RaftLogArray::get_entries_from(uint32_t start_index) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<LogEntry> result;

    if (start_index < base_index_)
    {
        return result;
    }

    size_t array_index = start_index - base_index_;
    for (size_t i = array_index; i < entries_.size(); ++i)
    {
        result.push_back(entries_[i]);
    }

    return result;
}
std::vector<LogEntry> RaftLogArray::get_entries_from(uint32_t start_index, int max_count) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<LogEntry> result;

    if (start_index < base_index_)
    {
        return result;
    }

    size_t array_index = start_index - base_index_;
    size_t end_index = array_index + max_count;
    for (size_t i = array_index; i < entries_.size() && i < end_index; ++i)
    {
        result.push_back(entries_[i]);
    }

    return result;
}
bool RaftLogArray::recover()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    LOG_INFO("Starting Raft log recovery...");

    // 1. 加载索引
    if (!load_index())
    {
        LOG_WARN("Failed to load index, starting with empty log");
        return true;
    }

    // 2. 加载日志条目
    if (!load_entries())
    {
        LOG_WARN("Failed to load entries, starting with empty log");
        entries_.clear();
        index_offsets_.clear();
        base_index_ = 1;
        return true;
    }

    // 根据加载的第一条日志恢复 base_index_
    if (!entries_.empty())
    {
        base_index_ = entries_.front().index;
        LOG_INFO("Recovered base_index_: %u", base_index_);
    }
    else
    {
        base_index_ = 1;
    }

    LOG_INFO("Recovered %zu log entries (index: %u-%u)",
             entries_.size(), base_index_, get_last_index());

    return true;
}

bool RaftLogArray::load_index()
{
    // 读取索引文件
    if (index_file_ == nullptr)
    {
        LOG_ERROR("Index file is not open");
        return false;
    }

    index_offsets_.clear();
    // 将文件指针移动到文件开头
    fseek(index_file_, 0, SEEK_SET);
    // 读取每个偏移量
    uint64_t offset;
    while (fread(&offset, sizeof(offset), 1, index_file_) == 1)
    {
        index_offsets_.push_back(offset);
    }
    // 将文件指针移动到文件末尾
    fseek(index_file_, 0, SEEK_END);
    LOG_INFO("Loaded %zu index entries", index_offsets_.size());
    return true;
}

bool RaftLogArray::load_entries()
{
    // 遍历索引，从 WAL 中读取日志条目
    entries_.clear();

    for (size_t i = 0; i < index_offsets_.size(); ++i)
    {
        LogEntry entry;
        uint64_t offset = index_offsets_[i];

        if (!read_entry_from_wal(offset, entry))
        {
            LOG_ERROR("Failed to read entry at index %zu (offset: %lu)", i, offset);
            return false;
        }

        entries_.push_back(entry);
    }

    return true;
}

bool RaftLogArray::truncate_from(uint32_t index)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (index < base_index_ || index > base_index_ + entries_.size())
    {
        LOG_ERROR("Invalid truncate index: %u (base: %u, size: %zu)",
                  index, base_index_, entries_.size());
        return false;
    }

    LOG_INFO("Truncating logs from index %u (removing entries >= %u)", index, index);
    if (index == base_index_ + entries_.size())
    {
        LOG_INFO("Truncate to the last index, no need to truncate");
        return true;
    }

    // 1. 截断内存中的日志 (删除 index 及之后的日志)
    size_t array_index = index - base_index_;
    if (array_index < entries_.size())
    {
        entries_.resize(array_index);
        index_offsets_.resize(array_index);
    }

    // 2. 截断 WAL 和索引文件 (创建新文件)
    truncate_wal_and_index();

    LOG_INFO("Truncated to %u entries (new last index: %u)",
             static_cast<uint32_t>(entries_.size()), get_last_index());

    return true;
}

bool RaftLogArray::truncate_before(uint32_t index)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (index < base_index_ || index > base_index_ + entries_.size())
    {
        LOG_ERROR("Invalid truncate_before index: %u (base: %u, size: %zu)",
                  index, base_index_, entries_.size());
        return false;
    }

    LOG_INFO("Truncating logs before index %u (removing entries < %u)", index, index);
    if (index == base_index_)
    {
        LOG_INFO("Truncate_before to base_index, no need to truncate");
        return true;
    }

    // 1. 截断内存中的日志 (删除 index 之前的日志)
    size_t array_index = index - base_index_;
    if (array_index > 0)
    {
        // 删除前 array_index 个元素
        entries_.erase(entries_.begin(), entries_.begin() + array_index);
        index_offsets_.erase(index_offsets_.begin(), index_offsets_.begin() + array_index);

        // 更新 base_index
        base_index_ = index;
    }

    // 2. 重建 WAL 和索引文件
    truncate_wal_and_index();

    LOG_INFO("Truncated_before to index %u (new base_index: %u, new last_index: %u)",
             index, base_index_, get_last_index());

    return true;
}

void RaftLogArray::truncate_wal_and_index()
{
    // 关闭当前文件
    if (wal_file_ != nullptr)
        fclose(wal_file_);
    if (index_file_ != nullptr)
        fclose(index_file_);

    // 备份旧文件
    std::string old_wal = wal_path_ + ".old";
    std::string old_idx = index_path_ + ".old";
    rename_file(wal_path_, old_wal);
    rename_file(index_path_, old_idx);

    // 创建新文件
    wal_file_ = fopen(wal_path_.c_str(), "wb+");
    index_file_ = fopen(index_path_.c_str(), "wb+");

    if (wal_file_ == nullptr || index_file_ == nullptr)
    {
        LOG_ERROR("Failed to create truncated files");
        return;
    }

    // 重新写入截断后的数据
    for (size_t i = 0; i < entries_.size(); ++i)
    {
        uint64_t offset;
        if (!write_entry_to_wal(entries_[i], offset))
        {
            LOG_ERROR("Failed to rewrite entry %zu to WAL", i);
            continue;
        }
        index_offsets_[i] = offset;
        write_index_entry(offset);
    }

    // 清理旧文件
    remove_file(old_wal);
    remove_file(old_idx);
}

bool RaftLogArray::create_snapshot(uint32_t snapshot_index)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (snapshot_index < base_index_ || snapshot_index >= base_index_ + entries_.size())
    {
        LOG_ERROR("Invalid snapshot index: %u (base: %u, size: %zu)",
                  snapshot_index, base_index_, entries_.size());
        return false;
    }

    LOG_INFO("Creating snapshot at index %u (will truncate logs <= %u)",
             snapshot_index, snapshot_index);

    // 截断 snapshot_index 之前的旧日志，保留之后的日志
    return truncate_before(snapshot_index + 1);
}

void RaftLogArray::flush()
{
    if (wal_file_ != nullptr)
        fflush(wal_file_);
    if (index_file_ != nullptr)
        fflush(index_file_);
}

void RaftLogArray::sync_metadata()
{
    flush();

    // 强制刷盘 (fsync)
#ifdef _WIN32
    if (wal_file_ != nullptr)
        _commit(_fileno(wal_file_));
    if (index_file_ != nullptr)
        _commit(_fileno(index_file_));
#else
    if (wal_file_ != nullptr)
        fsync(fileno(wal_file_));
    if (index_file_ != nullptr)
        fsync(fileno(index_file_));
#endif
}

uint32_t RaftLogArray::get_term(uint32_t index) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (index < base_index_ || index >= base_index_ + entries_.size())
    {
        return 0;
    }

    size_t array_index = index - base_index_;
    return entries_[array_index].term;
}

// 清空所有日志，并将 base_index 设置为 new_start_index
// 通常用于 InstallSnapshot 之后
bool RaftLogArray::reset(uint32_t new_start_index)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    LOG_INFO("Resetting LogArray to start from index %u", new_start_index);

    // 1. 清空内存
    entries_.clear();
    index_offsets_.clear();
    base_index_ = new_start_index;

    // 2. 关闭文件
    if (wal_file_)
        fclose(wal_file_);
    if (index_file_)
        fclose(index_file_);

    // 3. 删除旧文件 (直接删除再重建是最高效的清空方式)
    remove_file(wal_path_);
    remove_file(index_path_);

    // 4. 重新打开文件 (wb+ 模式会创建新文件)
    wal_file_ = fopen(wal_path_.c_str(), "wb+");
    index_file_ = fopen(index_path_.c_str(), "wb+");

    if (!wal_file_ || !index_file_)
    {
        LOG_ERROR("Failed to recreate log files during reset");
        return false;
    }

    return true;
}