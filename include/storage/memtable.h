#pragma once

#include <string>
#include <map>
#include <shared_mutex>
#include <optional>

namespace tinykv::storage {

/**
 * @brief MemTable 负责在内存中存储 Key-Value 数据。
 * 
 * 它是存储引擎的第一层，所有写入操作首先写入 MemTable。
 * 为了支持并发访问，内部使用了读写锁 (std::shared_mutex)。
 * 使用 std::map (红黑树) 保持 Key 有序，方便后续 Flush 到 SSTable。
 */
class MemTable {
public:
    MemTable() = default;
    ~MemTable() = default;

    // 禁止拷贝和赋值，避免意外的开销和锁问题
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    /**
     * @brief 插入或更新一个 Key-Value 对。
     * @param key 键
     * @param value 值
     */
    void Put(const std::string& key, const std::string& value);

    /**
     * @brief 获取指定 Key 的值。
     * @param key 键
     * @return 如果 Key 存在，返回对应的 Value；否则返回 std::nullopt。
     */
    std::optional<std::string> Get(const std::string& key) const;

    /**
     * @brief 删除指定 Key。
     * @param key 键
     */
    void Delete(const std::string& key);

    /**
     * @brief 获取当前 MemTable 中包含的元素数量（用于测试或监控）。
     */
    size_t Size() const;

    /**
     * @brief 清空 MemTable (主要用于测试或重置)。
     */
    void Clear();

private:
    // 核心数据结构，使用有序 Map
    std::map<std::string, std::string> table_;
    
    // 读写锁，保护 table_ 的并发访问
    // mutable 关键字允许在 const 成员函数 (如 Get) 中修改锁的状态 (lock/unlock)
    mutable std::shared_mutex mutex_;
};

} // namespace tinykv::storage