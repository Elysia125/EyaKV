#pragma once

#include <string>
#include <vector>
#include <map>

namespace tinykv::storage {

/**
 * @brief SSTable (Sorted String Table) 是磁盘上的不可变有序文件。
 * 
 * 当 MemTable 达到一定大小时，会 Flush 到磁盘生成 SSTable。
 * TODO: 目前仅作为占位符，后续阶段完善具体的文件格式和读写逻辑。
 */
class SSTable {
public:
    explicit SSTable(const std::string& filepath);
    ~SSTable() = default;

    /**
     * @brief 从 SSTable 中查找 Key。
     */
    bool Get(const std::string& key, std::string* value) const;

private:
    std::string filepath_;
};

/**
 * @brief 用于构建 SSTable 的辅助类。
 */
class SSTableBuilder {
public:
    explicit SSTableBuilder(const std::string& filepath);
    ~SSTableBuilder() = default;

    /**
     * @brief 添加 Key-Value 对。必须按 Key 升序添加。
     */
    void Add(const std::string& key, const std::string& value);

    /**
     * @brief 完成构建并关闭文件。
     */
    void Finish();

private:
    std::string filepath_;
};

} // namespace tinykv::storage
