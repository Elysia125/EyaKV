#include "storage/wal.h"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include <set>
#include <chrono>
#include "common/util/path_utils.h"
#include "logger/logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#else
#include <unistd.h> // 包含 dup, fdatasync, close (Linux/macOS)
#endif
#include <cstdio>

namespace fs = std::filesystem;

Wal::Wal(const std::string &wal_dir,
         const bool &sync_on_write) : wal_dir_(wal_dir),
                                      wal_file_(nullptr),
                                      sync_on_write_(sync_on_write),
                                      modified_(false)
{
    if (!std::filesystem::exists(wal_dir_))
    {
        std::filesystem::create_directories(wal_dir_);
    }
}

Wal::~Wal()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ != nullptr)
    {
        LOG_INFO("Wal::~Wal: Closing WAL file: {}", (PathUtils::combine_path(wal_dir_, wal_file_name_)).c_str());

        // 析构时必须同步写入，由于已经获取锁控制权且即将销毁，这里直接阻塞同步即可。
        if (modified_)
        {
            fflush(wal_file_);
#ifdef _WIN32
            int fd = _fileno(wal_file_);
            if (fd != -1)
                _commit(fd);
#else
            int fd = fileno(wal_file_);
            if (fd != -1)
                fdatasync(fd);
#endif
        }

        fclose(wal_file_);
        wal_file_ = nullptr;
        LOG_INFO("Wal::~Wal: WAL file closed");
    }
}

bool Wal::append_log(uint8_t type, const std::string &key, const std::string &payload)
{
    LOG_DEBUG("Wal: Appending log type={} key={}", type, key.c_str());
    return write_record(type, key, payload);
}

bool Wal::write_record(uint8_t type, const std::string &key, const std::string &payload)
{
    // 使用 thread_local buffer 避免了频繁的高并发动态内存分配操作 (new/delete)，提升热点性能。
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t payload_len = static_cast<uint32_t>(payload.size());

    size_t total_size = sizeof(type) + sizeof(key_len) + key_len + sizeof(payload_len) + payload_len;

    thread_local std::string buffer;
    buffer.clear();
    // 限制 thread_local buffer 的最大持续驻留容量，防止极端特大日志造成线程内存泄漏式增长 (1MB限制)
    if (buffer.capacity() > 1024 * 1024)
    {
        buffer.shrink_to_fit();
    }
    buffer.reserve(total_size);

    // 缓冲聚簇合并写入:
    buffer.append(reinterpret_cast<const char *>(&type), sizeof(type));
    buffer.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
    buffer.append(key);
    buffer.append(reinterpret_cast<const char *>(&payload_len), sizeof(payload_len));
    if (payload_len > 0)
    {
        buffer.append(payload);
    }

    bool write_success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (wal_file_ == nullptr)
            return false;

        size_t written = fwrite(buffer.data(), 1, buffer.size(), wal_file_);
        write_success = (written == buffer.size()) && !ferror(wal_file_);
        if (write_success)
        {
            modified_ = true;
        }
    }

    // 若要求每次写强刷盘，则调用 sync()。
    if (write_success && sync_on_write_)
    {
        return sync();
    }

    return write_success;
}

bool Wal::recover(std::function<void(std::string, uint8_t, std::string, std::string)> callback)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    LOG_INFO("Starting WAL recovery from directory: {}", wal_dir_.c_str());

    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }

    LOG_INFO("Wal::Recover: Scanning WAL directory...");
    std::set<std::string> wal_files;
    try
    {
        for (const auto &entry : fs::directory_iterator(wal_dir_))
        {
            if (entry.path().extension() == ".wal")
            {
                wal_files.insert(entry.path().string());
                LOG_INFO("Wal::Recover: Found WAL file: {}", entry.path().string().c_str());
            }
        }
        LOG_INFO("Wal::Recover: Found {} WAL files", wal_files.size());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Wal::Recover: Exception while scanning WAL directory: {}", e.what());
        return false;
    }

    // 重用反序列化缓存区:
    // 在读取大量旧日志时，重用 std::string，避免遍历每条日志时不断重新分配/释放大量内存。
    std::string key;
    std::string payload;

    for (const auto &filepath : wal_files)
    {
        LOG_INFO("Wal::Recover: Starting recovery from file: {}", filepath.c_str());

        try
        {
            size_t file_size = std::filesystem::file_size(filepath);
            LOG_INFO("Wal::Recover: WAL file size: {} bytes", file_size);
            if (file_size == 0)
            {
                LOG_INFO("Wal::Recover: WAL file is empty, deleting it: {}", filepath.c_str());
                std::filesystem::remove(filepath);
                continue;
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Wal::Recover: Exception while checking file size: {}", e.what());
            continue;
        }

        std::ifstream reader(filepath, std::ios::binary);
        if (!reader.is_open())
        {
            LOG_ERROR("Wal::Recover: Failed to open WAL file at {}", filepath.c_str());
            continue;
        }
        LOG_INFO("Wal::Recover: WAL file opened successfully");

        int record_count = 0;
        LOG_INFO("Wal::Recover: Starting to read records...");

        while (true)
        {
            uint8_t type_u8;
            if (!reader.read(reinterpret_cast<char *>(&type_u8), sizeof(type_u8)))
                break;

            uint32_t key_len;
            if (!reader.read(reinterpret_cast<char *>(&key_len), sizeof(key_len)))
                break;

            // 数据防腐保护：防止异常/损坏的数据导致的恶意或溢出性内存分配 (如Key限1MB)
            if (key_len > 1024 * 1024)
            {
                LOG_ERROR("Wal::Recover: Unreasonable key length {}, possibly corrupted.", key_len);
                break;
            }

            key.resize(key_len);
            if (!reader.read(&key[0], key_len))
                break;

            uint32_t val_len;
            if (!reader.read(reinterpret_cast<char *>(&val_len), sizeof(val_len)))
                break;

            // 数据防腐保护：防止有效载荷造成的进程被 OOM 终止 (限制128MB)
            if (val_len > 128 * 1024 * 1024)
            {
                LOG_ERROR("Wal::Recover: Unreasonable payload length {}, possibly corrupted.", val_len);
                break;
            }

            if (val_len > 0)
            {
                payload.resize(val_len);
                if (!reader.read(&payload[0], val_len))
                    break;
            }
            else
            {
                payload.clear();
            }

            LOG_DEBUG("Wal::Recover: Processing record {}, type: {}, key: {}", record_count, type_u8, key.c_str());

            // 值传递给回调时，此时编译器/执行路径会自动进行拷贝，充分保障外围回调函数处理安全
            callback(std::filesystem::path(filepath).filename().string(), type_u8, key, payload);
            record_count++;
        }

        LOG_INFO("Wal::Recover: Read {} records from file", record_count);
        reader.close();
        LOG_INFO("Wal::Recover: Completed recovery from WAL file: {}", filepath.c_str());
    }

    LOG_INFO("Wal::Recover: WAL recovery completed successfully.");
    return true;
}
bool Wal::clear(const std::string &filename)
{
    std::string filepath = PathUtils::combine_path(wal_dir_, filename);

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (wal_file_name_ == filename && wal_file_ != nullptr)
        {
            fclose(wal_file_);
            wal_file_ = nullptr;
            wal_file_name_.clear();
            modified_ = false;
        }
    }

    std::error_code ec;
    bool removed = std::filesystem::remove(filepath, ec);
    if (!removed)
    {
        LOG_ERROR("Wal::clear: Failed to remove file {}, error: {}", filepath.c_str(), ec.message().c_str());
    }
    return removed;
}

bool Wal::sync()
{
    int dup_fd = -1;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!modified_ || wal_file_ == nullptr)
        {
            return true; // 没有修改或文件尚未打开，避免无意义的 OS 调用
        }

        // 仅在锁内执行将 C 库空间缓冲区冲刷到操作系统内核区的轻量操作
        fflush(wal_file_);
        modified_ = false;

        // 无锁刷盘 (Lock-Free Disk Flush)
        // 通过使用文件描述符克隆 (dup)，允许我们在互斥锁作用域外部去执行重度阻塞的底层系统调用 (fdatasync/commit)
        // 这一操作能完美实现：在磁盘真正持久化落盘的高耗时阶段，互不干扰且允许其它写入线程获取互斥锁写内存。
#ifdef _WIN32
        int fd = _fileno(wal_file_);
        if (fd != -1)
            dup_fd = _dup(fd);
#else
        int fd = fileno(wal_file_);
        if (fd != -1)
            dup_fd = dup(fd);
#endif

        if (dup_fd == -1)
        {
            LOG_ERROR("Wal::sync: Failed to duplicate file descriptor for syncing.");
            return false;
        }
    }

    // 释放了 mutex_，进行昂贵的物理阻塞耗时操作
    bool success = false;
#ifdef _WIN32
    success = (_commit(dup_fd) == 0);
    _close(dup_fd);
#else
    // fdatasync 仅刷核心数据不强刷元数据（除非必要），比通用的 fsync 性能更好
    success = (fdatasync(dup_fd) == 0);
    close(dup_fd);
#endif

    if (!success)
    {
        LOG_ERROR("Wal::sync: Failed to sync WAL file to disk. Error: {}", strerror(errno));
    }

    return success;
}

void Wal::open_wal_file(std::string &filename)
{
    // 若当前有待刷盘的缓冲数据，必须先安全刷盘以防丢失
    sync();

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_file_ != nullptr)
    {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }

    if (filename.empty())
    {
        filename = generate_unique_filename();
    }

    std::string filepath = PathUtils::combine_path(wal_dir_, filename);
    wal_file_ = fopen(filepath.c_str(), "ab+");
    if (wal_file_ == nullptr)
    {
        LOG_ERROR("Wal: Failed to open WAL file at {}, error:{}", filepath.c_str(), strerror(errno));
        throw std::runtime_error("cannot open or create WAL file at " + filepath);
    }

    wal_file_name_ = filename;
    modified_ = false; // 新开启的文件无未处理的变更
    LOG_INFO("Wal: Opened WAL file at {}", filepath.c_str());
}

std::string Wal::open_wal_file()
{
    std::string filename = generate_unique_filename();
    open_wal_file(filename);
    return filename;
}

std::string Wal::generate_unique_filename()
{
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "eya_" + std::to_string(now) + ".wal";
}