#include "storage/wal.h"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include "common/path_utils.h"
#include "logger/logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#else
#include <unistd.h> // 包含fsync/fdatasync（Linux/macOS）
#endif
#include <cstdio>

#define WAL_FILE_NAME "eya.wal"

namespace fs = std::filesystem;
Wal::Wal(const std::string &wal_dir,
         const unsigned long &wal_file_size,
         const unsigned long &max_wal_file_count,
         const bool &sync_on_write) : wal_dir_(wal_dir),
                                      wal_file_size(wal_file_size * 1024),
                                      max_wal_file_count(max_wal_file_count),
                                      sync_on_write_(sync_on_write)
{
    std::string filepath = PathUtils::CombinePath(wal_dir_, WAL_FILE_NAME);
    if (!std::filesystem::exists(wal_dir_))
    {
        std::filesystem::create_directories(wal_dir_);
    }
    OpenWALFile();
}
Wal::~Wal()
{
    if (wal_file_ != nullptr)
    {
        LOG_DEBUG("Wal: Closing WAL file.");
        fflush(wal_file_);
        fclose(wal_file_);
    }
}

bool Wal::AppendPut(const std::string &key, const EyaValue &value)
{
    LOG_DEBUG("Wal: Appending Put record. Key: {}, Value: {}", key, value);
    return WriteRecord(LogType::kPut, key, value);
}

bool Wal::AppendDelete(const std::string &key)
{
    LOG_DEBUG("Wal: Appending Delete record. Key: {}", key);
    return WriteRecord(LogType::kDelete, key, "");
}

bool Wal::WriteRecord(LogType type, const std::string &key, const EyaValue &value)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ == nullptr)
        return false;

    // Simple format:
    // [Type (1B)] [KeyLen (4B)] [Key] [ValueLen (4B)] [Value]
    fseek(wal_file_, 0, SEEK_END);
    long file_size = ftell(wal_file_);
    if (file_size >= wal_file_size)
    {
        LOG_INFO("Wal: WAL file size reached max({} bytes). Switch wal file now.", wal_file_size);
        Sync();
        fclose(wal_file_);
        PathUtils::RenameFile(PathUtils::CombinePath(wal_dir_, WAL_FILE_NAME),
                              PathUtils::CombinePath(wal_dir_, "eya_" + std::to_string(std::time(nullptr)) + ".wal"));
        // 判断wal文件数是否达到上限
        fs::directory_iterator dir_iter(wal_dir_);
        fs::path oldest_file;
        std::time_t oldest_time = std::time(nullptr);
        size_t wal_file_count = 0;
        for (const auto &entry : dir_iter)
        {
            if (entry.path().extension() == ".wal")
            {
                wal_file_count++;
                std::time_t file_time = fs::last_write_time(entry.path()).time_since_epoch().count();
                if (file_time < oldest_time)
                {
                    oldest_time = file_time;
                    oldest_file = entry.path();
                }
            }
        }
        if (wal_file_count >= max_wal_file_count)
        {
            LOG_WARN("Wal: Maximum WAL file count ({}) reached. Delete oldest one automatically.", max_wal_file_count);
            // 删除创建时间最早的
            fs::remove(oldest_file);
        }
        OpenWALFile();
    }
    uint8_t type_u8 = static_cast<uint8_t>(type);
    uint32_t key_len = static_cast<uint32_t>(key.size());
    std::string value_str = serialize_eya_value(value);
    uint32_t value_len = static_cast<uint32_t>(value_str.size());
    fwrite(&type_u8, sizeof(type_u8), 1, wal_file_);
    fwrite(&key_len, sizeof(key_len), 1, wal_file_);
    fwrite(key.data(), key_len, 1, wal_file_);
    fwrite(&value_len, sizeof(value_len), 1, wal_file_);
    fwrite(value_str.data(), value_len, 1, wal_file_);
    modifyed_ = true;
    if (sync_on_write_)
    {
        Sync();
    }
    else
    {
        // 刷新到内核缓冲区
        fflush(wal_file_);
    }
    return !ferror(wal_file_);
}

bool Wal::Recover(MemTable<std::string, EyaValue> *memtable)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    LOG_INFO("Starting WAL recovery from directory: {}", wal_dir_);
    fclose(wal_file_);
    // 打开wal目录下的所有wal文件进行恢复
    fs::directory_iterator dir_iter(wal_dir_);
    for (const auto &entry : dir_iter)
    {
        if (entry.path().extension() == ".wal")
        {
            std::string filepath = entry.path().string();
            LOG_INFO("Recovering from WAL file: {}", filepath);
            std::ifstream reader(filepath, std::ios::binary);
            if (!reader.is_open())
            {
                LOG_ERROR("Wal::Recover: Failed to open WAL file at {}", filepath);
                continue;
            }

            while (reader.peek() != EOF)
            {
                uint8_t type_u8;
                uint32_t key_len;
                uint32_t val_len;

                reader.read(reinterpret_cast<char *>(&type_u8), sizeof(type_u8));
                if (reader.eof())
                    break;

                reader.read(reinterpret_cast<char *>(&key_len), sizeof(key_len));

                std::string key(key_len, '\0');
                reader.read(&key[0], key_len);

                reader.read(reinterpret_cast<char *>(&val_len), sizeof(val_len));

                char *val_data = new char[val_len];
                if (val_len > 0)
                {
                    reader.read(val_data, val_len);
                }
                size_t offset = 0;
                EyaValue value = deserialize_eya_value(val_data, offset);
                if (reader.fail())
                {
                    std::cerr << "Wal::Recover: Error reading log file, maybe truncated." << std::endl;
                    break;
                }

                LogType type = static_cast<LogType>(type_u8);
                if (type == LogType::kPut)
                {
                    memtable->put(key, value);
                }
                else if (type == LogType::kDelete)
                {
                    memtable->remove(key);
                }
            }

            reader.close();
            // 删除已恢复的日志文件
            std::filesystem::remove(filepath);
            LOG_INFO("Completed recovery from WAL file: {}", filepath);
        }
    }
    // Reopen for appending
    OpenWALFile();
    LOG_INFO("WAL recovery completed.");
    return wal_file_ != nullptr;
}

bool Wal::Clear()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
    }
    // std::string filepath = PathUtils::CombinePath(wal_dir_, WAL_FILE_NAME);
    // Truncate file
    // std::ofstream file(filepath, std::ios::out | std::ios::trunc | std::ios::binary);
    // file.close();
    // 删除wal目录下的所有wal文件
    fs::directory_iterator dir_iter(wal_dir_);
    for (const auto &entry : dir_iter)
    {
        if (entry.path().extension() == ".wal")
        {
            std::string filepath = entry.path().string();
            std::filesystem::remove(filepath);
            LOG_INFO("Wal: Deleted WAL file at {}", filepath);
        }
    }
    // Reopen
    OpenWALFile();
    return wal_file_ != nullptr;
}

bool Wal::Sync()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!modifyed_)
    {
        return true; // 没有修改，无需同步
    }
    if (wal_file_ != nullptr)
    {
        fflush(wal_file_);
#ifdef _WIN32
        int fd = _fileno(wal_file_);
        if (fd == -1)
        {
            LOG_ERROR("Wal: Failed to get file descriptor for syncing.");
            return false;
        }
        if (_commit(fd) != 0)
        {
            LOG_ERROR("Wal: Failed to sync WAL file to disk.");
            return false;
        }
        modifyed_ = false;
        return true;

#else
        // 步骤1：获取底层文件描述符
        int fd = fileno(wal_file_); // 从FILE*获取fd
        if (fd == -1)
        {
            LOG_ERROR("Wal: Failed to get file descriptor for syncing.");
            return false;
        }

        // 步骤2：调用fsync刷内核缓冲区到磁盘（真正落盘）
        if (fdatasync(fd) == -1)
        { // fdatasync(fd) 更高效（仅刷数据）
            LOG_ERROR("Wal: Failed to sync WAL file to disk. Error: {}", strerror(errno));
            return false;
        }
        modifyed_ = false;
        return true;
#endif
    }
    return false;
}

void Wal::OpenWALFile()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
    }
    std::string filepath = PathUtils::CombinePath(wal_dir_, WAL_FILE_NAME);
    wal_file_ = fopen(filepath.c_str(), "ab+");
    if (wal_file_ == nullptr)
    {
        LOG_ERROR("Wal: Failed to open WAL file at {},error:{}", filepath, strerror(errno));
        throw std::runtime_error("cannot open or create WAL file at " + filepath);
    }
}