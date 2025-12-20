#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include "storage/memtable.h"

namespace tinykv::storage {

/**
 * @brief Write-Ahead Log (WAL) 负责将操作持久化到磁盘。
 * 
 * 在写入 MemTable 之前，必须先写入 WAL。这样即使进程崩溃，
 * 重启后也能通过重放 WAL 来恢复 MemTable 中的数据。
 */
class Wal {
public:
    /**
     * @brief 构造函数，打开或创建指定的日志文件。
     * @param filepath 日志文件的路径
     */
    explicit Wal(const std::string& filepath);
    ~Wal();

    // 禁止拷贝
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    /**
     * @brief 记录 Put 操作。
     * @param key 键
     * @param value 值
     * @return 成功返回 true，失败返回 false
     */
    bool AppendPut(const std::string& key, const std::string& value);

    /**
     * @brief 记录 Delete 操作。
     * @param key 键
     * @return 成功返回 true，失败返回 false
     */
    bool AppendDelete(const std::string& key);

    /**
     * @brief 从日志文件中恢复数据到 MemTable。
     * 
     * 通常在系统启动时调用。会读取所有日志条目并重放到 MemTable 中。
     * @param memtable 指向需要恢复的 MemTable 对象的指针
     * @return 成功返回 true，失败返回 false
     */
    bool Recover(MemTable* memtable);

    /**
     * @brief 清空日志文件（例如在 Flush 到 SSTable 后）。
     */
    bool Clear();

private:
    std::string filepath_;
    std::fstream file_;
    std::mutex mutex_; // 保护文件写入的互斥锁

    // 日志记录类型
    enum class LogType : uint8_t {
        kPut = 1,
        kDelete = 2
    };

    // 内部辅助函数：写入一条日志记录
    bool WriteRecord(LogType type, const std::string& key, const std::string& value);
};

} // namespace tinykv::storage
