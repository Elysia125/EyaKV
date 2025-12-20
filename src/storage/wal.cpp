#include "storage/wal.h"
#include <iostream>
#include <filesystem>

namespace tinykv::storage {

Wal::Wal(const std::string& filepath) : filepath_(filepath) {
    // 以追加模式打开，如果文件不存在则创建
    // std::ios::in | std::ios::out | std::ios::app ensures we can read and append.
    // std::ios::binary is crucial for binary data.
    file_.open(filepath, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        // 尝试只创建文件 (out mode only) 如果上面的失败 (e.g. file didn't exist)
         file_.open(filepath, std::ios::out | std::ios::binary);
         file_.close();
         // Reopen in read/write/append mode
         file_.open(filepath, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    }
}

Wal::~Wal() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool Wal::AppendPut(const std::string& key, const std::string& value) {
    return WriteRecord(LogType::kPut, key, value);
}

bool Wal::AppendDelete(const std::string& key) {
    return WriteRecord(LogType::kDelete, key, "");
}

bool Wal::WriteRecord(LogType type, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open()) return false;

    // Simple format:
    // [Type (1B)] [KeyLen (4B)] [Key] [ValueLen (4B)] [Value]
    
    uint8_t type_u8 = static_cast<uint8_t>(type);
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());

    file_.write(reinterpret_cast<const char*>(&type_u8), sizeof(type_u8));
    file_.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
    file_.write(key.data(), key_len);
    file_.write(reinterpret_cast<const char*>(&val_len), sizeof(val_len));
    if (val_len > 0) {
        file_.write(value.data(), val_len);
    }

    // Flush to ensure data hits OS buffer (fsync for disk is stricter, but flush is okay for now)
    file_.flush();

    return !file_.fail();
}

bool Wal::Recover(MemTable* memtable) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Close current handle to reopen in read-only from start
    file_.close();
    
    std::ifstream reader(filepath_, std::ios::binary);
    if (!reader.is_open()) {
        // Reopen original handle
        file_.open(filepath_, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
        return false; // Could happen if file doesn't exist yet
    }

    while (reader.peek() != EOF) {
        uint8_t type_u8;
        uint32_t key_len;
        uint32_t val_len;

        reader.read(reinterpret_cast<char*>(&type_u8), sizeof(type_u8));
        if (reader.eof()) break;

        reader.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));
        
        std::string key(key_len, '\0');
        reader.read(&key[0], key_len);

        reader.read(reinterpret_cast<char*>(&val_len), sizeof(val_len));
        
        std::string value;
        if (val_len > 0) {
            value.resize(val_len);
            reader.read(&value[0], val_len);
        }

        if (reader.fail()) {
            std::cerr << "Wal::Recover: Error reading log file, maybe truncated." << std::endl;
            break; 
        }

        LogType type = static_cast<LogType>(type_u8);
        if (type == LogType::kPut) {
            memtable->Put(key, value);
        } else if (type == LogType::kDelete) {
            memtable->Delete(key);
        }
    }

    reader.close();
    // Reopen for appending
    file_.open(filepath_, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    
    return true;
}

bool Wal::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.close();
    // Truncate file
    file_.open(filepath_, std::ios::out | std::ios::trunc | std::ios::binary);
    file_.close();
    // Reopen
    file_.open(filepath_, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
    return file_.is_open();
}

} // namespace tinykv::storage
