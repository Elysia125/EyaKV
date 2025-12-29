#ifndef TINYKV_STORAGE_PROCESSOR_H_
#define TINYKV_STORAGE_PROCESSOR_H_

#include "storage/memtable.h"
#include <vector>
#include <string>
#include "common/common.h"
#include "storage/wal.h"
class Storage;

class ValueProcessor
{
public:
    virtual ~ValueProcessor() = default;

    virtual Result execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args) = 0;

    virtual bool recover(MemTable<std::string, EValue> *memtable,const uint8_t type, const std::string &key, const std::string &payload) = 0;
    /**
     * @brief 获取支持的类型
     */
    virtual std::vector<uint8_t> get_supported_types() const = 0;
};

#endif // TINYKV_STORAGE_PROCESSOR_H_
