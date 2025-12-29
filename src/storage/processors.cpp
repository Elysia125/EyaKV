#include "storage/processors/structure_processors.h"
#include "logger/logger.h"

// StringProcessor

std::vector<uint8_t> StringProcessor::get_supported_types() const
{
    return {LogType::kSet};
}

bool StringProcessor::set(MemTable<std::string, EValue> *memtable, Wal *wal, const std::string &key, const std::string &value, const uint64_t &ttl)
{
    EValue val;
    val.value = value;
    val.expire_time = ttl == 0 ? 0 : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + ttl;
    if (wal)
    {
        wal->append_log(LogType::kSet, key, serialize(val));
    }
    memtable->put(key, val);
    return true;
}

Result StringProcessor::execute(MemTable<std::string, EValue> *memtable, Wal *wal, const uint8_t type, const std::vector<std::string> &args)
{
    if (type == LogType::kSet)
    {
        if (args.size() < 2)
        {
            return Result::error("missing arguments");
        }
        return Result::success(std::string(set(memtable, wal, args[0], args[1], args.size() > 2 ? std::stoll(args[2]) : 0) ? "1" : "0"));
    }
    return Result::error("unsupported type");
}
bool StringProcessor::recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload)
{
    if (type == LogType::kSet)
    {
        size_t offset = 0;
        EValue val = deserialize(payload.data(), offset);
        memtable->put(key, val);
        return true;
    }
    return false;
}
// SetProcessor
std::vector<uint8_t> SetProcessor::get_supported_types() const
{
    return {LogType::kSAdd, LogType::kSRem, LogType::kSMembers};
}

void SetProcessor::recover(MemTable<std::string, EValue> *memtable, const uint8_t type, const std::string &key, const std::string &payload)
{
}

bool SetProcessor::sadd(Storage &storage, const std::string &key, const std::vector<std::string> &members)
{
}

bool SetProcessor::srem(Storage &storage, const std::string &key, const std::vector<std::string> &members)
{
}

// ==================== ZSetProcessor ====================

std::vector<uint8_t> ZSetProcessor::get_supported_types() const
{
    return {LogType::kZAdd, LogType::kZRem};
}

void ZSetProcessor::apply(MemTable<std::string, EValue> &memtable, uint8_t type, const std::string &key, const std::string &payload)
{
    if (type == LogType::kZAdd)
    {
        size_t offset = 0;
        double score = EyaKV::Serializer::deserializeDouble(payload.data(), offset);
        std::string member = EyaKV::Serializer::deserializeString(payload.data(), offset);

        try
        {
            memtable.handle_value(key, [&](EValue &v) -> EValue &
                                  {
                if (v.is_deleted() || v.is_expired()) {
                    v.value = ZSet();
                    v.deleted = false;
                    v.expire_time = 0;
                }
                if (!std::holds_alternative<ZSet>(v.value)) {
                    v.value = ZSet();
                }
                auto &zset = std::get<ZSet>(v.value);
                zset.add(score, member);
                return v; });
        }
        catch (const std::out_of_range &)
        {
            EValue val;
            ZSet zset;
            zset.add(score, member);
            val.value = zset;
            memtable.put(key, val);
        }
    }
    else if (type == LogType::kZRem)
    {
        size_t offset = 0;
        std::string member = EyaKV::Serializer::deserializeString(payload.data(), offset);

        try
        {
            memtable.handle_value(key, [&](EValue &v) -> EValue &
                                  {
                if (v.is_deleted() || v.is_expired()) return v;
                if (std::holds_alternative<ZSet>(v.value)) {
                    auto &zset = std::get<ZSet>(v.value);
                    zset.remove(member);
                }
                return v; });
        }
        catch (const std::out_of_range &)
        {
        }
    }
}

bool ZSetProcessor::zadd(Storage &storage, const std::string &key, double score, const std::string &member)
{
    std::string payload = EyaKV::Serializer::serialize(score) + EyaKV::Serializer::serialize(member);
    return storage.apply_log(LogType::kZAdd, key, payload);
}

bool ZSetProcessor::zrem(Storage &storage, const std::string &key, const std::string &member)
{
    return storage.apply_log(LogType::kZRem, key, EyaKV::Serializer::serialize(member));
}

// ==================== VectorProcessor ====================

std::vector<uint8_t> VectorProcessor::get_supported_types() const
{
    return {LogType::kLPush, LogType::kRPop}; // kRPush etc can be added
}

void VectorProcessor::apply(MemTable<std::string, EValue> &memtable, uint8_t type, const std::string &key, const std::string &payload)
{
    if (type == LogType::kLPush)
    {
        size_t offset = 0;
        std::string element = EyaKV::Serializer::deserializeString(payload.data(), offset);

        try
        {
            memtable.handle_value(key, [&](EValue &v) -> EValue &
                                  {
                if (v.is_deleted() || v.is_expired()) {
                    v.value = std::vector<std::string>();
                    v.deleted = false;
                    v.expire_time = 0;
                }
                if (!std::holds_alternative<std::vector<std::string>>(v.value)) {
                    v.value = std::vector<std::string>();
                }
                auto &vec = std::get<std::vector<std::string>>(v.value);
                vec.insert(vec.begin(), element);
                return v; });
        }
        catch (const std::out_of_range &)
        {
            EValue val;
            std::vector<std::string> vec;
            vec.push_back(element);
            val.value = vec;
            memtable.put(key, val);
        }
    }
    else if (type == LogType::kRPop)
    {
        try
        {
            memtable.handle_value(key, [&](EValue &v) -> EValue &
                                  {
               if (v.is_deleted() || v.is_expired()) return v;
               if (std::holds_alternative<std::vector<std::string>>(v.value)) {
                   auto &vec = std::get<std::vector<std::string>>(v.value);
                   if (!vec.empty()) vec.pop_back();
               }
               return v; });
        }
        catch (const std::out_of_range &)
        {
        }
    }
}

bool VectorProcessor::lpush(Storage &storage, const std::string &key, const std::string &element)
{
    return storage.apply_log(LogType::kLPush, key, EyaKV::Serializer::serialize(element));
}

std::optional<std::string> VectorProcessor::rpop(Storage &storage, const std::string &key)
{
    if (storage.read_only_)
        return std::nullopt;

    // 1. Write WAL
    if (storage.enable_wal_ && storage.wal_ && !storage.wal_->append_log(LogType::kRPop, key, ""))
    {
        return std::nullopt;
    }

    // 2. Update MemTable and Capture result
    std::optional<std::string> result;
    try
    {
        storage.memtable_->handle_value(key, [&](EValue &v) -> EValue &
                                        {
             if (v.is_deleted() || v.is_expired()) return v;
             if (std::holds_alternative<std::vector<std::string>>(v.value)) {
                   auto &vec = std::get<std::vector<std::string>>(v.value);
                   if (!vec.empty()) {
                       result = vec.back();
                       vec.pop_back();
                   }
               }
             return v; });
    }
    catch (...)
    {
    }

    // Check flush
    if (storage.memtable_->should_flush())
        storage.rotate_memtable();

    return result;
}
