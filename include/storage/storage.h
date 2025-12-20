#pragma once

#include <string>
#include <memory>
#include <optional>
#include "storage/memtable.h"
#include "storage/wal.h"

namespace tinykv::storage {

/**
 * @brief Storage 类是存储引擎的统一入口。
 * 
 * 它负责管理内存中的 MemTable、磁盘上的 WAL 以及未来的 SSTable。
 * 对外提供统一的 Put/Get/Delete 接口。
 */
class Storage {
public:
    /**
     * @brief 构造函数。
     * @param data_dir 数据存储目录，用于存放 WAL 和 SSTable 文件。
     */
    explicit Storage(const std::string& data_dir);
    ~Storage();

    // 禁止拷贝
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /**
     * @brief 写入数据。
     * 
     * 流程：
     * 1. 写入 WAL (持久化)
     * 2. 写入 MemTable (内存索引)
     */
    bool Put(const std::string& key, const std::string& value);

    /**
     * @brief 读取数据。
     * 
     * 流程：
     * 1. 查询 MemTable
     * 2. (TODO) 查询 Immutable MemTable
     * 3. (TODO) 查询 SSTable
     */
    std::optional<std::string> Get(const std::string& key) const;

    /**
     * @brief 删除数据。
     * 
     * 流程：
     * 1. 写入 WAL (Tombstone)
     * 2. 在 MemTable 中标记删除或直接移除
     */
    bool Delete(const std::string& key);

private:
    std::string data_dir_;
    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<Wal> wal_;

    // 初始化时恢复数据
    void Recover();
};

} // namespace tinykv::storage
