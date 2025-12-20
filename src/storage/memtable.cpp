#include "storage/memtable.h"
#include <mutex> // for std::unique_lock, std::shared_lock

namespace tinykv::storage {

void MemTable::Put(const std::string& key, const std::string& value) {
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_[key] = value;
}

std::optional<std::string> MemTable::Get(const std::string& key) const {
    // 获取读锁 (共享锁)
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.find(key);
    if (it != table_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void MemTable::Delete(const std::string& key) {
    // 获取写锁 (独占锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.erase(key);
}

size_t MemTable::Size() const {
    // 获取读锁
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

void MemTable::Clear() {
    // 获取写锁
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
}

} // namespace tinykv::storage