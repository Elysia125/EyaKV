#include "storage/wal.h"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include <set>
#include "common/path_utils.h"
#include "logger/logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#else
#include <unistd.h> // 包含fsync/fdatasync（Linux/macOS）
#endif
#include <cstdio>

namespace fs = std::filesystem;
Wal::Wal(const std::string &wal_dir,
         const bool &sync_on_write) : wal_dir_(wal_dir),
                                      sync_on_write_(sync_on_write)
{
    if (!std::filesystem::exists(wal_dir_))
    {
        std::filesystem::create_directories(wal_dir_);
    }
}
Wal::~Wal()
{
    if (wal_file_ != nullptr)
    {
        LOG_DEBUG("Wal: Closing WAL file.");
        sync();
        fclose(wal_file_);
    }
}

bool Wal::append_log(uint8_t type, const std::string &key, const std::string &payload)
{
    LOG_DEBUG("Wal: Appending log to WAL file.");
    return write_record(type, key, payload);
}

bool Wal::write_record(uint8_t type, const std::string &key, const std::string &payload)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ == nullptr)
        return false;

    // Simple format:
    // [Type (1B)] [KeyLen (4B)] [Key] [PayloadLen (4B)] [Payload]
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t payload_len = static_cast<uint32_t>(payload.size());

    fwrite(&type, sizeof(type), 1, wal_file_);
    fwrite(&key_len, sizeof(key_len), 1, wal_file_);
    fwrite(key.data(), key_len, 1, wal_file_);
    fwrite(&payload_len, sizeof(payload_len), 1, wal_file_);
    if (payload_len > 0)
    {
        fwrite(payload.data(), payload_len, 1, wal_file_);
    }
    modifyed_ = true;
    if (sync_on_write_)
    {
        sync();
    }
    else
    {
        // 刷新到内核缓冲区
        fflush(wal_file_);
    }
    return !ferror(wal_file_);
}

bool Wal::recover(std::function<void(std::string, uint8_t, std::string, std::string)> callback)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    LOG_INFO("Starting WAL recovery from directory: %s", wal_dir_.c_str());
    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
    }
    // 打开wal目录下的所有wal文件进行恢复
    fs::directory_iterator dir_iter(wal_dir_);
    std::set<std::string> wal_files;
    for (const auto &entry : dir_iter)
    {
        if (entry.path().extension() == ".wal")
        {
            wal_files.insert(entry.path().string());
        }
    }
    for (const auto &filepath : wal_files)
    {
        LOG_INFO("Recovering from WAL file: %s", filepath.c_str());
        std::ifstream reader(filepath, std::ios::binary);
        if (!reader.is_open())
        {
            LOG_ERROR("Wal::Recover: Failed to open WAL file at %s", filepath.c_str());
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

            // 使用 std::vector 自动管理内存，避免内存泄漏
            std::vector<char> val_data(val_len);
            if (val_len > 0)
            {
                reader.read(val_data.data(), val_len);
            }

            if (reader.fail())
            {
                std::cerr << "Wal::Recover: Error reading log file " << filepath << ", maybe truncated." << std::endl;
                break;
            }

            // Call generic callback
            std::string payload(val_data.begin(), val_data.end());
            callback(std::filesystem::path(filepath).filename().string(), type_u8, key, payload);
        }

        reader.close();
        // 删除已恢复的日志文件
        // std::filesystem::remove(filepath);
        LOG_INFO("Completed recovery from WAL file: %s", filepath);
    }
    // Reopen for appending
    // open_wal_file();
    LOG_INFO("WAL recovery completed.");
    return true;
}

bool Wal::clear(const std::string &filename)
{
    std::string filepath = PathUtils::CombinePath(wal_dir_, filename);
    if (wal_file_name_ == filename && wal_file_ != nullptr)
    {
        sync();
        fclose(wal_file_);
        // open_wal_file();
    }
    return std::filesystem::remove(filepath);
}

bool Wal::sync()
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
            LOG_ERROR("Wal: Failed to sync WAL file to disk. Error: %s", strerror(errno));
            return false;
        }
        modifyed_ = false;
        return true;
#endif
    }
    return false;
}

std::string Wal::open_wal_file(std::optional<std::string> filename)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ != nullptr)
    {
        sync();
        fclose(wal_file_);
    }
    if (filename == std::nullopt || filename.value().empty())
    {
        filename = generate_unique_filename();
    }
    std::string filepath = PathUtils::CombinePath(wal_dir_, filename.value());
    wal_file_ = fopen(filepath.c_str(), "ab+");
    if (wal_file_ == nullptr)
    {
        LOG_ERROR("Wal: Failed to open WAL file at %s, error:%s", filepath.c_str(), strerror(errno));
        throw std::runtime_error("cannot open or create WAL file at " + filepath);
    }
    wal_file_name_ = filename.value();
    return filename.value();
}

std::string Wal::generate_unique_filename()
{
    return "eya_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".wal";
}