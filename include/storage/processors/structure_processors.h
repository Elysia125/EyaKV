#ifndef TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
#define TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_

#include "storage/processors/processor.h"
#include "storage/storage.h"
#include "common/serializer.h"
#include "common/common.h"

// String Processor
class StringProcessor : public ValueProcessor
{
private:
    // Operations
    bool set(MemTable<std::string, EValue> *memtable, Wal *wal, const std::string &key, const std::string &value, const uint64_t &ttl = 0);

public:
    Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) override;
    bool recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload) override;
    std::vector<uint8_t> get_supported_types() const override;
};

// Set Processor
class SetProcessor : public ValueProcessor
{
public:
    Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) override;
    std::vector<uint8_t> get_supported_types() const override;
    bool recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload) override;

    // Operations
    bool sadd(Storage &storage, const std::string &key, const std::vector<std::string> &members);
    bool srem(Storage &storage, const std::string &key, const std::vector<std::string> &members);
};

// ZSet Processor
class ZSetProcessor : public ValueProcessor
{
public:
    Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) override;
    std::vector<uint8_t> get_supported_types() const override;
    bool recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload) override;

    // Operations
    bool zadd(Storage &storage, const std::string &key, double score, const std::string &member);
    bool zrem(Storage &storage, const std::string &key, const std::string &member);
};

// Deque (List) Processor
class DequeProcessor : public ValueProcessor
{
public:
    Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) override;
    std::vector<uint8_t> get_supported_types() const override;
    bool recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload) override;

    // Operations
    bool lpush(Storage &storage, const std::string &key, const std::string &element);
    std::optional<std::string> rpop(Storage &storage, const std::string &key);
};

// Map Processor
class MapProcessor : public ValueProcessor
{
public:
    Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) override;
    std::vector<uint8_t> get_supported_types() const override;
    bool recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload) override;
};

#endif // TINYKV_STORAGE_STRUCTURE_PROCESSORS_H_
