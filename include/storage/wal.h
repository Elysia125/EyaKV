#ifndef WAL_H_
#define WAL_H_

#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <functional>
#include "storage/memtable.h"
#include "common/types/value.h"

/**
 * @class Wal
 * @brief Write-Ahead Log (WAL) 负责将操作持久化到磁盘。
 *
 * 在写入 MemTable 之前，必须先写入 WAL。这样即使进程崩溃，
 * 重启后也能通过重放 WAL 来恢复 MemTable 中的数据。
 * 本实现经过高度优化，支持高速序列化、并发写入与无锁刷盘 (Lock-Free Sync)。
 */
class Wal
{
public:
    /**
     * @brief 构造函数，打开或创建指定的日志文件目录。
     * @param wal_dir 日志文件存放所在的目录路径
     * @param sync_on_write 是否在每次写入后立刻同步到磁盘（强持久化模式）
     */
    explicit Wal(const std::string &wal_dir,
                 const bool &sync_on_write = false);

    /**
     * @brief 析构函数，负责安全地关闭并同步尚未落盘的日志文件。
     */
    ~Wal();

    // 禁止拷贝与赋值
    Wal(const Wal &) = delete;
    Wal &operator=(const Wal &) = delete;

    /**
     * @brief 记录通用操作日志到 WAL 中。
     * @param type 操作类型（例如 Put/Delete）
     * @param key 键的字符串
     * @param payload 序列化后的载荷或值
     * @return 写入（若开启强一致则包括刷盘）成功返回 true，失败返回 false
     */
    bool append_log(uint8_t type, const std::string &key, const std::string &payload);

    /**
     * @brief 从日志文件中按顺序恢复数据。
     *
     * 通常在系统启动时调用，会遍历目录下的所有 .wal 文件并按时间戳顺序重放日志。
     * @param callback 用于接收每一条合法日志记录的回调函数
     * @return 恢复成功返回 true，遭遇无法恢复的灾难性错误返回 false
     */
    bool recover(std::function<void(std::string filename, uint8_t type, std::string key, std::string payload)> callback);

    /**
     * @brief 清空并删除指定的日志文件（例如在 MemTable 成功 Flush 到 SSTable 后调用）。
     * @param filename 要直接删除的日志文件名
     * @return 删除成功返回 true
     */
    bool clear(const std::string &filename);

    /**
     * @brief 同步日志文件到磁盘，确保内核缓冲区的数据持久化到底层存储设备。
     * @return 同步成功返回 true
     */
    bool sync();

    /**
     * @brief 开启一个指定的 WAL 文件。如果传入的文件名为空，则内部会生成一个新文件名。
     * @param filename 传入引用，如果为空，将被赋值为新生成的文件名
     */
    void open_wal_file(std::string &filename);

    /**
     * @brief 开启一个新的 WAL 文件并返回其基于时间戳生成的文件名。
     * @return 新生成的 WAL 文件名
     */
    std::string open_wal_file();

private:
    const std::string wal_dir_;        ///< WAL 文件存放目录
    FILE *wal_file_;                   ///< 当前正在写入的 WAL 文件底层句柄
    std::string wal_file_name_;        ///< 当前 WAL 文件的短名称
    std::recursive_mutex mutex_;       ///< 保护文件句柄和缓冲区的可重入互斥锁
    const bool sync_on_write_ = false; ///< 记录构造函数传入的是否强刷盘的标志
    bool modified_ = false;            ///< 记录自上次刷盘以来是否有新的内存修改（用于避免无谓I/O）

    /**
     * @brief 内部辅助函数：将一条操作记录组装序列化并写入文件。
     * @param type 操作类型
     * @param key 键
     * @param payload 载荷
     * @return 写入成功返回 true
     */
    bool write_record(uint8_t type, const std::string &key, const std::string &payload);

    /**
     * @brief 生成唯一的 WAL 文件名。
     * 基于高精度时间戳生成，确保了字母序排列即为时间顺序。
     * @return 生成的唯一文件名字符串
     */
    std::string generate_unique_filename();
};

#endif // WAL_H_