#include "storage/storage.h"
#include <filesystem>
#include <iostream>

namespace tinykv::storage {

Storage::Storage(const std::string& data_dir) : data_dir_(data_dir) {
    // 确保数据目录存在
    if (!std::filesystem::exists(data_dir_)) {
        std::filesystem::create_directories(data_dir_);
    }

    // 初始化 MemTable
    memtable_ = std::make_unique<MemTable>();

    // 初始化 WAL
    // 假设只有一个 active wal 文件
    std::string wal_path = data_dir_ + "/wal.log";
    wal_ = std::make_unique<Wal>(wal_path);

    // 恢复数据
    Recover();
}

Storage::~Storage() = default;

void Storage::Recover() {
    if (wal_ && memtable_) {
        bool success = wal_->Recover(memtable_.get());
        if (!success) {
            std::cerr << "Storage Warning: WAL recovery may have been incomplete." << std::endl;
        }
    }
}

bool Storage::Put(const std::string& key, const std::string& value) {
    // 1. 写 WAL
    if (!wal_->AppendPut(key, value)) {
        return false;
    }
    // 2. 写 MemTable
    memtable_->Put(key, value);
    return true;
}

std::optional<std::string> Storage::Get(const std::string& key) const {
    // 1. 查 MemTable
    auto result = memtable_->Get(key);
    if (result.has_value()) {
        return result;
    }
    
    // TODO: 查 SSTable
    return std::nullopt;
}

bool Storage::Delete(const std::string& key) {
    // 1. 写 WAL
    if (!wal_->AppendDelete(key)) {
        return false;
    }
    // 2. 更新 MemTable
    memtable_->Delete(key);
    return true;
}

} // namespace tinykv::storage
