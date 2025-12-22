#include "storage/sstable.h"
#include <iostream>

namespace eyakv::storage
{

    SSTable::SSTable(const std::string &filepath) : filepath_(filepath)
    {
        // TODO: 打开文件，读取索引
    }

    bool SSTable::Get(const std::string &key, std::string *value) const
    {
        // TODO: 实现二分查找或索引查找
        return false;
    }

    SSTableBuilder::SSTableBuilder(const std::string &filepath) : filepath_(filepath)
    {
        // TODO: 打开文件准备写入
    }

    void SSTableBuilder::Add(const std::string &key, const std::string &value)
    {
        // TODO: 写入 Key-Value 数据块
    }

    void SSTableBuilder::Finish()
    {
        // TODO: 写入元数据和索引块，关闭文件
    }

} // namespace eyakv::storage
