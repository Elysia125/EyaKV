#include "storage/processors/structure_processors.h"
#include "logger/logger.h"
#include "storage/storage.h"
#include "common/serialization/serializer.h"
#include "common/types/operation_type.h"
#include "common/types/key_encoder.h"
#include <limits>
#define MAX_INLINE_SIZE 64        // 元素小于64时直接内嵌在Metadata中，避免额外的内存分配和指针间接访问
#define FIXED_DEQUE_CHUNK_SIZE 64 // List元素超过64个时，分块存储，每块64个元素，形成链表结构
// StringProcessor
std::vector<uint8_t> StringProcessor::get_supported_types() const
{
    return {OperationType::kSet};
}

bool StringProcessor::set(Storage *storage, const std::string &key, const std::string &value, const uint64_t &ttl)
{
    EValue val;
    val.value = value;
    val.expire_time = ttl == 0 ? 0 : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + ttl;
    if (storage->enable_wal_ && storage->wal_)
    {
        storage->wal_->append_log(OperationType::kSet, key, serialize(val)); // WAL needs plain key
    }
    storage->write_memtable(key, val);
    return true;
}

bool StringProcessor::set(Storage *storage, const std::string_view key, const std::string_view value, const uint64_t &ttl)
{
    return set(storage, std::string(key), std::string(value), ttl);
}

Response StringProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (type == OperationType::kSet)
    {
        if (args.size() < 2)
            return Response::error("missing arguments");
        return Response::success(set(storage, args[0], args[1], args.size() > 2 ? std::stoll(args[2]) : 0));
    }
    return Response::error("unsupported type");
}

Response StringProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args)
{
    if (type == OperationType::kSet)
    {
        if (args.size() < 2)
            return Response::error("missing arguments");
        uint64_t ttl = args.size() > 2 ? std::stoll(std::string(args[2])) : 0;
        return Response::success(set(storage, args[0], args[1], ttl));
    }
    return Response::error("unsupported type");
}

bool StringProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    if (type == OperationType::kSet)
    {
        size_t offset = 0;
        EValue val = deserialize(payload.data(), offset);
        storage->write_memtable(key, val);
        return true;
    }
    return false;
}

// SetProcessor
std::vector<uint8_t> SetProcessor::get_supported_types() const
{
    return {OperationType::kSAdd, OperationType::kSRem, OperationType::kSMembers};
}

Response SetProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string key = args[0];

    switch (type)
    {
    case OperationType::kSAdd:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        std::vector<std::string> members;
        for (size_t i = 1; i < args.size(); ++i)
            members.push_back(args[i]);
        return Response::success(s_add(storage, key, members));
    }
    case OperationType::kSRem:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        std::vector<std::string> members;
        for (size_t i = 1; i < args.size(); ++i)
            members.push_back(args[i]);
        return Response::success(s_rem(storage, key, members));
    }
    case OperationType::kSMembers:
        return Response::success(s_members(storage, key));
    default:
        return Response::error("unsupported type");
    }
}

Response SetProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string_view key = args[0];

    switch (type)
    {
    case OperationType::kSAdd:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        std::vector<std::string_view> members;
        for (size_t i = 1; i < args.size(); ++i)
            members.push_back(args[i]);
        return Response::success(s_add(storage, key, members));
    }
    case OperationType::kSRem:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        std::vector<std::string_view> members;
        for (size_t i = 1; i < args.size(); ++i)
            members.push_back(args[i]);
        return Response::success(s_rem(storage, key, members));
    }
    case OperationType::kSMembers:
        return Response::success(s_members(storage, key));
    default:
        return Response::error("unsupported type");
    }
}

bool SetProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    if (type == OperationType::kSAdd)
    {
        std::vector<std::string> members;
        while (offset < payload.size())
            members.push_back(Serializer::deserializeString(payload.data(), offset));
        s_add(storage, key, members, true);
        return true;
    }
    else if (type == OperationType::kSRem)
    {
        std::vector<std::string> members;
        while (offset < payload.size())
            members.push_back(Serializer::deserializeString(payload.data(), offset));
        s_rem(storage, key, members, true);
        return true;
    }
    return false;
}

bool SetProcessor::set_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kSet);
}

void SetProcessor::set_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a set");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kSet))
            throw std::runtime_error("value is not a set");
    }
    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kSet);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = std::vector<std::string>(); // 初始使用 embeded_data 存储小集合
    }
}

bool SetProcessor::set_read_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kSet);
}

void SetProcessor::set_get_or_create_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a set");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kSet))
            throw std::runtime_error("value is not a set");
    }
    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kSet);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = std::vector<std::string>(); // 初始使用 embeded_data 存储小集合
    }
}
size_t SetProcessor::s_add(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");

    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &m : members)
            payload += Serializer::serialize(m);
        storage->wal_->append_log(OperationType::kSAdd, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    set_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::vector<std::pair<std::string, EValue>> batch;
    size_t added = 0;
    if (std::holds_alternative<std::vector<std::string>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::string>>(meta.embeded_data);
        for (const auto &m : members)
        {
            auto it = std::find(vec.begin(), vec.end(), m);
            if (it == vec.end())
            {
                vec.push_back(m);
                added++;
            }
        }
        meta.size = vec.size();

        if (vec.size() >= MAX_INLINE_SIZE)
        {
            // Migrating to external
            for (const auto &m : vec)
            {
                std::string sub_key = KeyEncoder::encode_set_sub_key(key, meta.version, m);
                EValue sub_val;
                sub_val.value = "";
                // storage->write_memtable(sub_key, sub_val);
                batch.emplace_back(sub_key, sub_val);
            }
            meta.embeded_data = std::monostate();
        }
    }
    else
    {
        // External storage
        for (const auto &m : members)
        {
            std::string sub_key = KeyEncoder::encode_set_sub_key(key, meta.version, m);
            std::optional<EValue> existing_val = storage->get_raw(sub_key);
            if (!existing_val.has_value() || existing_val->is_deleted())
            {
                EValue sub_val;
                sub_val.value = "";
                // storage->write_memtable(sub_key, sub_val);
                batch.emplace_back(sub_key, sub_val);
                added++;
            }
        }
        meta.size += added;
    }

    if (added > 0 || is_new)
    {
        EValue updated_meta;
        updated_meta.value = meta;
        if (!is_new && meta_val.has_value())
            updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return added;
}

size_t SetProcessor::s_add(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover)
{
    std::vector<std::string> string_members;
    for (auto m : members)
        string_members.emplace_back(m);
    return s_add(storage, std::string(key), string_members, is_recover);
}

size_t SetProcessor::s_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");

    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &m : members)
            payload += Serializer::serialize(m);
        storage->wal_->append_log(OperationType::kSRem, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!set_read_meta(storage, key, meta, meta_val))
        return 0;
    std::vector<std::pair<std::string, EValue>> batch;
    size_t removed = 0;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        for (const auto &m : members)
        {
            std::string sub_key = KeyEncoder::encode_set_sub_key(key, meta.version, m);
            std::optional<EValue> existing_val = storage->get_raw(sub_key);
            if (existing_val.has_value() && !existing_val->is_deleted())
            {
                EValue sub_ev("", true);
                // storage->write_memtable(sub_key, sub_ev);
                batch.emplace_back(sub_key, sub_ev);
                removed++;
            }
        }
    }
    else
    {
        auto &existing_members = std::get<std::vector<std::string>>(meta.embeded_data);
        for (const auto &m : members)
        {
            auto it = std::find(existing_members.begin(), existing_members.end(), m);
            if (it != existing_members.end())
            {
                existing_members.erase(it);
                removed++;
            }
        }
        meta.size = existing_members.size();
    }

    if (removed > 0)
    {
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return removed;
}

size_t SetProcessor::s_rem(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover)
{
    std::vector<std::string> string_members;
    for (auto m : members)
        string_members.emplace_back(m);
    return s_rem(storage, std::string(key), string_members, is_recover);
}

std::vector<std::string> SetProcessor::s_members(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!set_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::string> result;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kSet, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);

        auto kv_pairs = storage->range(prefix, end_prefix);
        result.reserve(kv_pairs.size());
        for (const auto &pair : kv_pairs)
        {
            result.emplace_back(KeyEncoder::decode_set_member(pair.first));
        }
    }
    else
    {
        auto &existing_members = std::get<std::vector<std::string>>(meta.embeded_data);
        result.insert(result.end(), existing_members.begin(), existing_members.end());
    }

    return result;
}

std::vector<std::string> SetProcessor::s_members(Storage *storage, const std::string &key)
{
    return s_members(storage, std::string_view(key));
}

// ZSetProcessor
std::vector<uint8_t> ZSetProcessor::get_supported_types() const
{
    return {OperationType::kZAdd, OperationType::kZRem, OperationType::kZScore, OperationType::kZRank, OperationType::kZCard, OperationType::kZIncrBy, OperationType::kZRangeByRank, OperationType::kZRangeByScore, OperationType::kZRemByRank, OperationType::kZRemByScore};
}

Response ZSetProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string key = args[0];

    switch (type)
    {
    case OperationType::kZAdd:
        if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            return Response::error("wrong number of arguments for 'zadd'");
        {
            std::vector<std::pair<std::string, std::string>> score_members;
            for (size_t i = 1; i < args.size(); i += 2)
                score_members.emplace_back(args[i], args[i + 1]);
            return Response::success(z_add(storage, key, score_members));
        }
    case OperationType::kZRem:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            std::vector<std::string> members;
            for (size_t i = 1; i < args.size(); ++i)
                members.push_back(args[i]);
            return Response::success(z_rem(storage, key, members));
        }
    case OperationType::kZScore:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            auto score = z_score(storage, key, args[1]);
            return score.has_value() ? Response::success(score.value()) : Response::error("not found");
        }
    case OperationType::kZRank:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            auto rank = z_rank(storage, key, args[1]);
            return rank.has_value() ? Response::success(std::to_string(rank.value())) : Response::error("not found");
        }
    case OperationType::kZCard:
        return Response::success(std::to_string(z_card(storage, key)));
    case OperationType::kZIncrBy:
        if (args.size() < 3)
            return Response::error("missing increment or member");
        return Response::success(z_incr_by(storage, key, args[1], args[2]));
    case OperationType::kZRangeByRank:
        if (args.size() < 3)
            return Response::error("missing start or end");
        return Response::success(z_range_by_rank(storage, key, std::stoll(args[1]), std::stoll(args[2])));
    case OperationType::kZRangeByScore:
        if (args.size() < 3)
            return Response::error("missing min or max");
        return Response::success(z_range_by_score(storage, key, args[1], args[2]));
    case OperationType::kZRemByRank:
        if (args.size() < 3)
            return Response::error("missing start or end");
        return Response::success(z_rem_by_rank(storage, key, std::stoll(args[1]), std::stoll(args[2])));
    case OperationType::kZRemByScore:
        if (args.size() < 3)
            return Response::error("missing min or max");
        return Response::success(z_rem_by_score(storage, key, args[1], args[2]));
    default:
        return Response::error("unsupported type");
    }
}

Response ZSetProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string_view key = args[0];

    switch (type)
    {
    case OperationType::kZAdd:
        if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            return Response::error("wrong number of arguments for 'zadd'");
        {
            std::vector<std::pair<std::string_view, std::string_view>> score_members;
            for (size_t i = 1; i < args.size(); i += 2)
                score_members.emplace_back(args[i], args[i + 1]);
            return Response::success(z_add(storage, key, score_members));
        }
    case OperationType::kZRem:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            std::vector<std::string_view> members;
            for (size_t i = 1; i < args.size(); ++i)
                members.push_back(args[i]);
            return Response::success(z_rem(storage, key, members));
        }
    case OperationType::kZScore:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            auto score = z_score(storage, key, args[1]);
            return score.has_value() ? Response::success(score.value()) : Response::error("not found");
        }
    case OperationType::kZRank:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            auto rank = z_rank(storage, key, args[1]);
            return rank.has_value() ? Response::success(std::to_string(rank.value())) : Response::error("not found");
        }
    case OperationType::kZCard:
        return Response::success(std::to_string(z_card(storage, key)));
    case OperationType::kZIncrBy:
        if (args.size() < 3)
            return Response::error("missing increment or member");
        return Response::success(z_incr_by(storage, key, args[1], args[2]));
    case OperationType::kZRangeByRank:
        if (args.size() < 3)
            return Response::error("missing start or end");
        return Response::success(z_range_by_rank(storage, key, std::stoll(std::string(args[1])), std::stoll(std::string(args[2]))));
    case OperationType::kZRangeByScore:
        if (args.size() < 3)
            return Response::error("missing min or max");
        return Response::success(z_range_by_score(storage, key, args[1], args[2]));
    case OperationType::kZRemByRank:
        if (args.size() < 3)
            return Response::error("missing start or end");
        return Response::success(z_rem_by_rank(storage, key, std::stoll(std::string(args[1])), std::stoll(std::string(args[2]))));
    case OperationType::kZRemByScore:
        if (args.size() < 3)
            return Response::error("missing min or max");
        return Response::success(z_rem_by_score(storage, key, args[1], args[2]));
    default:
        return Response::error("unsupported type");
    }
}

bool ZSetProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    switch (type)
    {
    case OperationType::kZAdd:
    {
        std::vector<std::pair<std::string, std::string>> score_members;
        while (offset < payload.size())
        {
            auto score = Serializer::deserializeString(payload.data(), offset);
            auto member = Serializer::deserializeString(payload.data(), offset);
            score_members.emplace_back(score, member);
        }
        z_add(storage, key, score_members, true);
        break;
    }
    case OperationType::kZRem:
    {
        std::vector<std::string> members;
        while (offset < payload.size())
        {
            auto member = Serializer::deserializeString(payload.data(), offset);
            members.push_back(member);
        }
        z_rem(storage, key, members, true);
        break;
    }
    case OperationType::kZIncrBy:
    {
        auto increment = Serializer::deserializeString(payload.data(), offset);
        auto member = Serializer::deserializeString(payload.data(), offset);
        z_incr_by(storage, key, increment, member, true);
        break;
    }
    case OperationType::kZRemByRank:
    {
        auto start = Serializer::deserializeString(payload.data(), offset);
        auto end = Serializer::deserializeString(payload.data(), offset);
        z_rem_by_rank(storage, key, std::stoll(start), std::stoll(end), true);
        break;
    }
    case OperationType::kZRemByScore:
    {
        auto min = Serializer::deserializeString(payload.data(), offset);
        auto max = Serializer::deserializeString(payload.data(), offset);
        z_rem_by_score(storage, key, min, max, true);
        break;
    }
    default:
        break;
    }
    return true;
}

void ZSetProcessor::zset_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a zset");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kZSet))
            throw std::runtime_error("value is not a zset");
    }
    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kZSet);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = ZSet();
    }
}

bool ZSetProcessor::zset_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kZSet);
}

void ZSetProcessor::zset_get_or_create_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a zset");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kZSet))
            throw std::runtime_error("value is not a zset");
    }
    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kZSet);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = ZSet();
    }
}

bool ZSetProcessor::zset_read_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kZSet);
}

size_t ZSetProcessor::z_add(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &score_members, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");

    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &p : score_members)
            payload += Serializer::serialize(p.first) + Serializer::serialize(p.second);
        storage->wal_->append_log(OperationType::kZAdd, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    zset_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::vector<std::pair<std::string, EValue>> batch;
    size_t added_count = 0;

    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        for (const auto &p : score_members)
        {
            double score = std::stod(p.first);
            const std::string &member = p.second;

            std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
            std::optional<EValue> existing_val = storage->get_raw(lookup_key);
            if (existing_val.has_value() && !existing_val->is_deleted() && std::holds_alternative<std::string>(existing_val->value))
            {
                double old_score = std::stod(std::get<std::string>(existing_val->value));
                std::string old_sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, old_score, member);
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(old_sort_key, del_val);
                batch.emplace_back(old_sort_key, del_val);
            }
            else
            {
                added_count++;
            }

            EValue score_val;
            score_val.value = p.first;
            // storage->write_memtable(lookup_key, score_val);
            batch.emplace_back(lookup_key, score_val);
            std::string sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, score, member);
            EValue sort_val;
            sort_val.value = std::string("");
            // storage->write_memtable(sort_key, sort_val);
            batch.emplace_back(sort_key, sort_val);
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        size_t old_sz = zset.size();
        for (const auto &p : score_members)
        {
            zset.zadd(p.second, p.first);
        }
        added_count = zset.size() - old_sz;

        if (zset.size() > MAX_INLINE_SIZE)
        {
            // Migrating to external
            zset.for_each([&](const std::string &member, double score)
                          {
                std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
                EValue score_val; score_val.value = std::to_string(score);
                //storage->write_memtable(lookup_key, score_val);
                batch.emplace_back(lookup_key, score_val);
                std::string sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, score, member);
                EValue sort_val; sort_val.value = std::string("");
                //storage->write_memtable(sort_key, sort_val); 
                batch.emplace_back(sort_key, sort_val); });
            meta.embeded_data = std::monostate();
        }
    }

    if (added_count > 0 || is_new)
    {
        meta.size += added_count;
        EValue updated_meta;
        updated_meta.value = meta;
        if (!is_new && meta_val.has_value())
            updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return added_count;
}

size_t ZSetProcessor::z_rem(Storage *storage, const std::string &key, const std::vector<std::string> &members, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");

    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &m : members)
            payload += Serializer::serialize(m);
        storage->wal_->append_log(OperationType::kZRem, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return 0;
    std::vector<std::pair<std::string, EValue>> batch;
    size_t rem_count = 0;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        for (const auto &member : members)
        {
            std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
            std::optional<EValue> existing_val = storage->get_raw(lookup_key);
            if (existing_val.has_value() && !existing_val->is_deleted() && std::holds_alternative<std::string>(existing_val->value))
            {
                double score = std::stod(std::get<std::string>(existing_val->value));
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(lookup_key, del_val);
                batch.emplace_back(lookup_key, del_val);
                std::string sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, score, member);
                // storage->write_memtable(sort_key, del_val);
                batch.emplace_back(sort_key, del_val);
                rem_count++;
            }
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        for (const auto &member : members)
        {
            if (zset.zrem(member))
                rem_count++;
        }
    }

    if (rem_count > 0)
    {
        meta.size -= rem_count;
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return rem_count;
}

std::optional<std::string> ZSetProcessor::z_score(Storage *storage, const std::string &key, const std::string &member)
{
    return z_score(storage, std::string_view(key), std::string_view(member));
}

std::optional<size_t> ZSetProcessor::z_rank(Storage *storage, const std::string &key, const std::string &member)
{
    return z_rank(storage, std::string_view(key), std::string_view(member));
}

size_t ZSetProcessor::z_card(Storage *storage, const std::string &key)
{
    return z_card(storage, std::string_view(key));
}

std::string ZSetProcessor::z_incr_by(Storage *storage, const std::string &key, const std::string &increment, const std::string &member, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(increment) + Serializer::serialize(member);
        storage->wal_->append_log(OperationType::kZIncrBy, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    zset_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::optional<std::string> new_score_str;
    std::vector<std::pair<std::string, EValue>> batch;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        double incr = std::stod(increment);
        double new_score = incr;
        std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
        std::optional<EValue> existing_val = storage->get_raw(lookup_key);

        if (existing_val.has_value() && !existing_val->is_deleted() && std::holds_alternative<std::string>(existing_val->value))
        {
            double old_score = std::stod(std::get<std::string>(existing_val->value));
            new_score = old_score + incr;
            std::string old_sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, old_score, member);
            EValue del_val;
            del_val.deleted = true;
            // storage->write_memtable(old_sort_key, del_val);
            batch.emplace_back(old_sort_key, del_val);
        }
        else
        {
            meta.size++;
        }

        new_score_str = std::to_string(new_score);
        EValue score_val;
        score_val.value = new_score_str.value();
        // storage->write_memtable(lookup_key, score_val);
        batch.emplace_back(lookup_key, score_val);
        std::string new_sort_key = KeyEncoder::encode_zset_sort_key(key, meta.version, new_score, member);
        EValue sort_val;
        sort_val.value = std::string("");
        // storage->write_memtable(new_sort_key, sort_val);
        batch.emplace_back(new_sort_key, sort_val);
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        new_score_str = zset.zincrby(member, increment);
    }

    EValue updated_meta;
    updated_meta.value = meta;
    if (!is_new && meta_val.has_value())
        updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return new_score_str.has_value() ? new_score_str.value() : "0";
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_rank(Storage *storage, const std::string &key, long long start, long long end)
{
    return z_range_by_rank(storage, std::string_view(key), start, end);
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max)
{
    return z_range_by_score(storage, std::string_view(key), std::string_view(min), std::string_view(max));
}

size_t ZSetProcessor::z_rem_by_rank(Storage *storage, const std::string &key, long long start, long long end, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(std::to_string(start)) + Serializer::serialize(std::to_string(end));
        storage->wal_->append_log(OperationType::kZRemByRank, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return 0;

    long long sz = static_cast<long long>(meta.size);
    if (start < 0)
        start += sz;
    if (end < 0)
        end += sz;
    if (start < 0)
        start = 0;
    if (end >= sz)
        end = sz - 1;
    if (start > end)
        return 0;
    std::vector<std::pair<std::string, EValue>> batch;
    size_t count = 0;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kZSetRank, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);

        long long rank = 0;
        for (const auto &pair : kv_pairs)
        {
            if (rank > end)
                break;
            if (rank >= start)
            {
                auto [score, member] = KeyEncoder::decode_zset_sort_key(pair.first);
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(std::string(pair.first), del_val);
                batch.emplace_back(std::string(pair.first), del_val);
                std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
                // storage->write_memtable(lookup_key, del_val);
                batch.emplace_back(lookup_key, del_val);
                count++;
            }
            rank++;
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        count = zset.zrem_range_by_rank(start, end);
    }

    if (count > 0)
    {
        meta.size -= count;
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return count;
}

size_t ZSetProcessor::z_rem_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max, const bool is_recover)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(min) + Serializer::serialize(max);
        storage->wal_->append_log(OperationType::kZRemByScore, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return 0;
    std::vector<std::pair<std::string, EValue>> batch;
    size_t count = 0;

    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        double min_score = std::stod(min);
        double max_score = std::stod(max);

        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kZSetRank, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);

        for (const auto &pair : kv_pairs)
        {
            auto [score, member] = KeyEncoder::decode_zset_sort_key(pair.first);
            if (score > max_score)
                break;
            if (score >= min_score)
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(std::string(pair.first), del_val);
                batch.emplace_back(std::string(pair.first), del_val);
                std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
                // storage->write_memtable(lookup_key, del_val);
                batch.emplace_back(lookup_key, del_val);
                count++;
            }
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        count = zset.zrem_range_by_score(min, max);
    }

    if (count > 0)
    {
        meta.size -= count;
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return count;
}

// string_view version wrapper calls
size_t ZSetProcessor::z_add(Storage *storage, const std::string_view key, const std::vector<std::pair<std::string_view, std::string_view>> &score_members, const bool is_recover)
{
    std::vector<std::pair<std::string, std::string>> str_sm;
    for (auto &p : score_members)
        str_sm.emplace_back(std::string(p.first), std::string(p.second));
    return z_add(storage, std::string(key), str_sm, is_recover);
}

size_t ZSetProcessor::z_rem(Storage *storage, const std::string_view key, const std::vector<std::string_view> &members, const bool is_recover)
{
    std::vector<std::string> str_m;
    for (auto &m : members)
        str_m.emplace_back(m);
    return z_rem(storage, std::string(key), str_m, is_recover);
}

std::optional<std::string> ZSetProcessor::z_score(Storage *storage, const std::string_view key, const std::string_view member)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return std::nullopt;

    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
        std::optional<EValue> val = storage->get_raw(lookup_key);
        if (val.has_value() && !val->is_deleted() && std::holds_alternative<std::string>(val->value))
            return std::get<std::string>(val->value);
    }
    else
    {
        auto &zset = std::get<ZSet>(meta.embeded_data);
        auto score = zset.zscore(member);
        if (score.has_value())
            return score.value();
    }
    return std::nullopt;
}

std::optional<size_t> ZSetProcessor::z_rank(Storage *storage, const std::string_view key, const std::string_view member)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return std::nullopt;

    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        std::string lookup_key = KeyEncoder::encode_zset_lookup_key(key, meta.version, member);
        std::optional<EValue> score_val = storage->get_raw(lookup_key);
        if (!score_val.has_value() || score_val->is_deleted() || !std::holds_alternative<std::string>(score_val->value))
            return std::nullopt;

        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kZSetRank, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);

        size_t rank = 0;
        for (const auto &pair : kv_pairs)
        {
            auto [score, m] = KeyEncoder::decode_zset_sort_key(pair.first);
            if (m == member)
                return rank;
            rank++;
        }
    }
    else
    {
        auto &zset = std::get<ZSet>(meta.embeded_data);
        auto rank = zset.zrank(member);
        if (rank >= 0)
            return rank;
    }
    return std::nullopt;
}

size_t ZSetProcessor::z_card(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return 0;
    return meta.size;
}

std::string ZSetProcessor::z_incr_by(Storage *storage, const std::string_view key, const std::string_view increment, const std::string_view member, const bool is_recover)
{
    return z_incr_by(storage, std::string(key), std::string(increment), std::string(member), is_recover);
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_rank(Storage *storage, const std::string_view key, long long start, long long end)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return {};

    long long sz = static_cast<long long>(meta.size);
    if (start < 0)
        start += sz;
    if (end < 0)
        end += sz;
    if (start < 0)
        start = 0;
    if (end >= sz)
        end = sz - 1;
    if (start > end)
        return {};

    std::vector<std::pair<std::string, EyaValue>> result;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kZSetRank, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);

        long long rank = 0;
        for (const auto &pair : kv_pairs)
        {
            if (rank > end)
                break;
            if (rank >= start)
            {
                auto [score, member] = KeyEncoder::decode_zset_sort_key(pair.first);
                result.push_back({member, std::to_string(score)});
            }
            rank++;
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        std::vector<std::pair<std::string, std::string>> zset_result = zset.zrange_by_rank(start, end);
        for (const auto &pair : zset_result)
            result.push_back({pair.first, pair.second});
    }
    return result;
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_score(Storage *storage, const std::string_view key, const std::string_view min, const std::string_view max)
{
    if (starts_with(key, KeyEncoder::FIXED_PREFIX))
        throw std::runtime_error("invalid key");
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!zset_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::pair<std::string, EyaValue>> result;
    if (std::holds_alternative<std::monostate>(meta.embeded_data))
    {
        double min_score = std::stod(std::string(min));
        double max_score = std::stod(std::string(max));

        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kZSetRank, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);

        for (const auto &pair : kv_pairs)
        {
            auto [score, member] = KeyEncoder::decode_zset_sort_key(pair.first);
            if (score > max_score)
                break;
            if (score >= min_score)
                result.push_back({member, std::to_string(score)});
        }
    }
    else
    {
        ZSet &zset = std::get<ZSet>(meta.embeded_data);
        std::vector<std::pair<std::string, std::string>> zset_result = zset.zrange_by_score(min, max);
        for (const auto &pair : zset_result)
            result.push_back({pair.first, pair.second});
    }
    return result;
}

size_t ZSetProcessor::z_rem_by_rank(Storage *storage, const std::string_view key, long long start, long long end, const bool is_recover)
{
    return z_rem_by_rank(storage, std::string(key), start, end, is_recover);
}

size_t ZSetProcessor::z_rem_by_score(Storage *storage, const std::string_view key, const std::string_view min, const std::string_view max, const bool is_recover)
{
    return z_rem_by_score(storage, std::string(key), std::string(min), std::string(max), is_recover);
}

// DequeProcessor (List)

std::vector<uint8_t> DequeProcessor::get_supported_types() const
{
    return {OperationType::kLPush, OperationType::kLPop, OperationType::kRPush, OperationType::kRPop, OperationType::kLRange, OperationType::kLGet, OperationType::kLSize, OperationType::kLPopN, OperationType::kRPopN};
}

Response DequeProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string key = args[0];

    switch (type)
    {
    case OperationType::kLPush:
    case OperationType::kRPush:
    {
        if (args.size() < 2)
            return Response::error("missing value");
        std::vector<std::string> values;
        for (size_t i = 1; i < args.size(); ++i)
            values.push_back(args[i]);
        return type == OperationType::kLPush ? Response::success(std::to_string(l_push(storage, key, values)))
                                             : Response::success(std::to_string(r_push(storage, key, values)));
    }
    case OperationType::kLPop:
    case OperationType::kRPop:
    {
        if (args.size() == 1)
        {
            auto v = type == OperationType::kLPop ? l_pop(storage, key) : r_pop(storage, key);
            return v.has_value() ? Response::success(v.value()) : Response::error("empty");
        }
        else
        {
            size_t count = std::stoull(args[1]);
            return type == OperationType::kLPop ? Response::success(l_pop_n(storage, key, count))
                                                : Response::success(r_pop_n(storage, key, count));
        }
    }
    case OperationType::kLSize:
        return Response::success(std::to_string(l_size(storage, key)));
    case OperationType::kLRange:
        if (args.size() < 3)
            return Response::error("missing start/end");
        return Response::success(l_range(storage, key, std::stoll(args[1]), std::stoll(args[2])));
    case OperationType::kLGet:
        if (args.size() < 2)
            return Response::error("missing index");
        {
            auto v = l_get(storage, key, std::stoll(args[1]));
            return v.has_value() ? Response::success(v.value()) : Response::error("not found");
        }
    case OperationType::kLPopN:
    case OperationType::kRPopN:
        if (args.size() < 2)
            return Response::error("missing count");
        return type == OperationType::kLPopN ? Response::success(l_pop_n(storage, key, std::stoll(args[1])))
                                             : Response::success(r_pop_n(storage, key, std::stoll(args[1])));
    default:
        return Response::error("unsupported type");
    }
}

Response DequeProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args)
{
    std::vector<std::string> str_args;
    for (auto v : args)
        str_args.emplace_back(v);
    return execute(storage, type, str_args);
}

bool DequeProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    if (type == OperationType::kLPush || type == OperationType::kRPush)
    {
        std::vector<std::string> values;
        Serializer::deserializeVector(payload.data(), offset, values);
        if (type == OperationType::kLPush)
            l_push(storage, key, values, true);
        else
            r_push(storage, key, values, true);
    }
    else if (type == OperationType::kLPop)
        l_pop(storage, key, true);
    else if (type == OperationType::kRPop)
        r_pop(storage, key, true);
    else if (type == OperationType::kLPopN || type == OperationType::kRPopN)
    {
        std::string count_str = Serializer::deserializeString(payload.data(), offset);
        if (type == OperationType::kLPopN)
            l_pop_n(storage, key, std::stoll(count_str), true);
        else
            r_pop_n(storage, key, std::stoll(count_str), true);
    }
    else
        return false;
    return true;
}

bool DequeProcessor::deque_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kList);
}

void DequeProcessor::deque_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a list");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kList))
            throw std::runtime_error("value is not a list");
    }

    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kList);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.head_seq = (uint64_t)1ULL << 32;
        meta.tail_seq = meta.head_seq;
        meta.embeded_data = std::deque<std::string>();
    }
}

bool DequeProcessor::deque_read_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kList);
}

void DequeProcessor::deque_get_or_create_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a list");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kList))
            throw std::runtime_error("value is not a list");
    }

    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kList);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.head_seq = (uint64_t)1ULL << 32;
        meta.tail_seq = meta.head_seq;
        meta.embeded_data = std::deque<std::string>();
    }
}

size_t DequeProcessor::l_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kLPush, key, Serializer::serialize(values));

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    deque_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::vector<std::pair<std::string, EValue>> batch;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        for (const auto &v : values)
            dq.push_front(v);
        meta.size += values.size();

        if (dq.size() > MAX_INLINE_SIZE)
        {
            ListElement chunk;
            chunk.seq = meta.head_seq;
            chunk.values = std::move(dq);
            EValue save_val;
            save_val.value = chunk;
            // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
            batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
            meta.embeded_data = std::monostate();
        }
    }
    else
    {
        std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq);
        std::optional<EValue> chunk_val = storage->get_raw(sub_key);
        ListElement chunk;
        if (chunk_val.has_value() && !chunk_val->is_deleted())
            chunk = std::get<ListElement>(chunk_val->value);
        else
            chunk.seq = meta.head_seq;

        for (const auto &v : values)
        {
            if (chunk.values.size() >= FIXED_DEQUE_CHUNK_SIZE)
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
                batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
                meta.head_seq--;
                chunk.seq = meta.head_seq;
                chunk.values.clear();
            }
            chunk.values.push_front(v);
            meta.size++;
        }
        EValue save_val;
        save_val.value = chunk;
        // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
        batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq), save_val);
    }

    EValue updated_meta;
    updated_meta.value = meta;
    if (!is_new && meta_val.has_value())
        updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return meta.size;
}

size_t DequeProcessor::r_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kRPush, key, Serializer::serialize(values));

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    deque_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::vector<std::pair<std::string, EValue>> batch;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        for (const auto &v : values)
            dq.push_back(v);
        meta.size += values.size();

        if (dq.size() > MAX_INLINE_SIZE)
        {
            ListElement chunk;
            chunk.seq = meta.tail_seq;
            chunk.values = std::move(dq);
            EValue save_val;
            save_val.value = chunk;
            // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
            batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
            meta.embeded_data = std::monostate();
        }
    }
    else
    {
        std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq);
        std::optional<EValue> chunk_val = storage->get_raw(sub_key);
        ListElement chunk;
        if (chunk_val.has_value() && !chunk_val->is_deleted())
            chunk = std::get<ListElement>(chunk_val->value);
        else
            chunk.seq = meta.tail_seq;

        for (const auto &v : values)
        {
            if (chunk.values.size() >= FIXED_DEQUE_CHUNK_SIZE)
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
                batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
                meta.tail_seq++;
                chunk.seq = meta.tail_seq;
                chunk.values.clear();
            }
            chunk.values.push_back(v);
            meta.size++;
        }
        EValue save_val;
        save_val.value = chunk;
        // storage->write_memtable(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
        batch.emplace_back(KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq), save_val);
    }

    EValue updated_meta;
    updated_meta.value = meta;
    if (!is_new && meta_val.has_value())
        updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return meta.size;
}

std::optional<std::string> DequeProcessor::l_pop(Storage *storage, const std::string &key, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kLPop, key, "");

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return std::nullopt;
    std::vector<std::pair<std::string, EValue>> batch;
    std::optional<std::string> result = std::nullopt;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        if (!dq.empty())
        {
            result = dq.front();
            dq.pop_front();
            meta.size--;
        }
    }
    else
    {
        std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq);
        std::optional<EValue> chunk_val = storage->get_raw(sub_key);
        if (chunk_val.has_value() && !chunk_val->is_deleted())
        {
            ListElement chunk = std::get<ListElement>(chunk_val->value);
            result = chunk.values.front();
            chunk.values.pop_front();
            meta.size--;

            if (chunk.values.empty())
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(sub_key, del_val);
                batch.emplace_back(sub_key, del_val);
                if (meta.head_seq < meta.tail_seq)
                    meta.head_seq++;
            }
            else
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(sub_key, save_val);
                batch.emplace_back(sub_key, save_val);
            }
        }
    }

    if (result.has_value())
    {
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return result;
}

std::optional<std::string> DequeProcessor::r_pop(Storage *storage, const std::string &key, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kRPop, key, "");

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return std::nullopt;
    std::vector<std::pair<std::string, EValue>> batch;
    std::optional<std::string> result = std::nullopt;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        if (!dq.empty())
        {
            result = dq.back();
            dq.pop_back();
            meta.size--;
        }
    }
    else
    {
        std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq);
        std::optional<EValue> chunk_val = storage->get_raw(sub_key);
        if (chunk_val.has_value() && !chunk_val->is_deleted())
        {
            ListElement chunk = std::get<ListElement>(chunk_val->value);
            result = chunk.values.back();
            chunk.values.pop_back();
            meta.size--;

            if (chunk.values.empty())
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(sub_key, del_val);
                batch.emplace_back(sub_key, del_val);
                if (meta.tail_seq > meta.head_seq)
                    meta.tail_seq--;
            }
            else
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(sub_key, save_val);
                batch.emplace_back(sub_key, save_val);
            }
        }
    }

    if (result.has_value())
    {
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return result;
}

std::vector<std::string> DequeProcessor::l_range(Storage *storage, const std::string &key, long long start, long long end)
{
    return l_range(storage, std::string_view(key), start, end);
}

std::optional<std::string> DequeProcessor::l_get(Storage *storage, const std::string &key, long long index)
{
    return l_get(storage, std::string_view(key), index);
}

size_t DequeProcessor::l_size(Storage *storage, const std::string &key)
{
    return l_size(storage, std::string_view(key));
}

std::vector<std::string> DequeProcessor::l_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kLPopN, key, Serializer::serialize(std::to_string(n)));

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::string> popped;
    size_t actual_n = std::min(n, static_cast<size_t>(meta.size));
    if (actual_n == 0)
        return {};
    std::vector<std::pair<std::string, EValue>> batch;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        for (size_t i = 0; i < actual_n; ++i)
        {
            popped.push_back(dq.front());
            dq.pop_front();
        }
        meta.size -= actual_n;
    }
    else
    {
        while (popped.size() < actual_n)
        {
            std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq);
            std::optional<EValue> chunk_val = storage->get_raw(sub_key);
            if (!chunk_val.has_value() || chunk_val->is_deleted())
            {
                if (meta.head_seq < meta.tail_seq)
                    meta.head_seq++;
                else
                    break;
                continue;
            }
            ListElement chunk = std::get<ListElement>(chunk_val->value);
            while (!chunk.values.empty() && popped.size() < actual_n)
            {
                popped.push_back(chunk.values.front());
                chunk.values.pop_front();
            }
            if (chunk.values.empty())
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(sub_key, del_val);
                batch.emplace_back(sub_key, del_val);
                if (meta.head_seq < meta.tail_seq)
                    meta.head_seq++;
            }
            else
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(sub_key, save_val);
                batch.emplace_back(sub_key, save_val);
            }
        }
        meta.size -= popped.size();
    }

    EValue updated_meta;
    updated_meta.value = meta;
    updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return popped;
}

std::vector<std::string> DequeProcessor::r_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
        storage->wal_->append_log(OperationType::kRPopN, key, Serializer::serialize(std::to_string(n)));

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::string> popped;
    size_t actual_n = std::min(n, static_cast<size_t>(meta.size));
    if (actual_n == 0)
        return {};
    std::vector<std::pair<std::string, EValue>> batch;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        for (size_t i = 0; i < actual_n; ++i)
        {
            popped.push_back(dq.back());
            dq.pop_back();
        }
        meta.size -= actual_n;
    }
    else
    {
        while (popped.size() < actual_n)
        {
            std::string sub_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.tail_seq);
            std::optional<EValue> chunk_val = storage->get_raw(sub_key);
            if (!chunk_val.has_value() || chunk_val->is_deleted())
            {
                if (meta.tail_seq > meta.head_seq)
                    meta.tail_seq--;
                else
                    break;
                continue;
            }
            ListElement chunk = std::get<ListElement>(chunk_val->value);
            while (!chunk.values.empty() && popped.size() < actual_n)
            {
                popped.push_back(chunk.values.back());
                chunk.values.pop_back();
            }
            if (chunk.values.empty())
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(sub_key, del_val);
                batch.emplace_back(sub_key, del_val);
                if (meta.tail_seq > meta.head_seq)
                    meta.tail_seq--;
            }
            else
            {
                EValue save_val;
                save_val.value = chunk;
                // storage->write_memtable(sub_key, save_val);
                batch.emplace_back(sub_key, save_val);
            }
        }
        meta.size -= popped.size();
    }

    EValue updated_meta;
    updated_meta.value = meta;
    updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return popped;
}

// string_view delegate methods
size_t DequeProcessor::l_push(Storage *storage, const std::string_view key, const std::vector<std::string_view> &values, const bool is_recover)
{
    std::vector<std::string> str_values;
    for (auto &v : values)
        str_values.emplace_back(v);
    return l_push(storage, std::string(key), str_values, is_recover);
}

size_t DequeProcessor::r_push(Storage *storage, const std::string_view key, const std::vector<std::string_view> &values, const bool is_recover)
{
    std::vector<std::string> str_values;
    for (auto &v : values)
        str_values.emplace_back(v);
    return r_push(storage, std::string(key), str_values, is_recover);
}

std::optional<std::string> DequeProcessor::l_pop(Storage *storage, const std::string_view key, const bool is_recover)
{
    return l_pop(storage, std::string(key), is_recover);
}
std::optional<std::string> DequeProcessor::r_pop(Storage *storage, const std::string_view key, const bool is_recover)
{
    return r_pop(storage, std::string(key), is_recover);
}
std::vector<std::string> DequeProcessor::l_range(Storage *storage, const std::string_view key, long long start, long long end)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return {};

    long long sz = static_cast<long long>(meta.size);
    if (start < 0)
        start += sz;
    if (end < 0)
        end += sz;
    if (start < 0)
        start = 0;
    if (end >= sz)
        end = sz - 1;
    if (start > end)
        return {};

    std::vector<std::string> result;
    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        for (long long i = start; i <= end; ++i)
            result.push_back(dq[i]);
    }
    else
    {
        // 1. 获取首块大小作为基准点
        std::string head_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq);
        std::optional<EValue> head_val = storage->get_raw(head_key);
        if (!head_val.has_value() || head_val->is_deleted())
            return result;

        ListElement head_chunk = std::get<ListElement>(head_val->value);
        long long S_h = head_chunk.values.size();

        uint64_t start_seq, end_seq;
        long long start_idx, end_idx;

        // 2. 计算 start 所在的块 (start_seq) 和 块内偏移 (start_idx)
        if (start < S_h)
        {
            start_seq = meta.head_seq;
            start_idx = start;
        }
        else
        {
            long long rem = start - S_h;
            start_seq = meta.head_seq + 1 + (rem / FIXED_DEQUE_CHUNK_SIZE);
            start_idx = rem % FIXED_DEQUE_CHUNK_SIZE;
        }

        // 3. 计算 end 所在的块 (end_seq) 和 块内偏移 (end_idx)
        if (end < S_h)
        {
            end_seq = meta.head_seq;
            end_idx = end;
        }
        else
        {
            long long rem = end - S_h;
            end_seq = meta.head_seq + 1 + (rem / FIXED_DEQUE_CHUNK_SIZE);
            end_idx = rem % FIXED_DEQUE_CHUNK_SIZE;
        }

        // 4. 只遍历需要的这几个块，精准提取
        for (uint64_t seq = start_seq; seq <= end_seq; ++seq)
        {
            ListElement current_chunk;
            if (seq == meta.head_seq)
            {
                current_chunk = head_chunk; // 复用刚才读出来的首块
            }
            else
            {
                std::string target_key = KeyEncoder::encode_list_sub_key(key, meta.version, seq);
                std::optional<EValue> target_val = storage->get_raw(target_key);
                if (target_val.has_value() && !target_val->is_deleted())
                {
                    current_chunk = std::get<ListElement>(target_val->value);
                }
                else
                {
                    continue; // 数据异常/缺失，安全跳过
                }
            }

            // 确定在本块中提取的边界
            long long s = (seq == start_seq) ? start_idx : 0;
            long long e = (seq == end_seq) ? end_idx : current_chunk.values.size() - 1;

            // 存入结果
            for (long long i = s; i <= e && i < current_chunk.values.size(); ++i)
            {
                result.push_back(current_chunk.values[i]);
            }
        }
    }
    return result;
}
std::optional<std::string> DequeProcessor::l_get(Storage *storage, const std::string_view key, long long index)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return std::nullopt;

    long long sz = static_cast<long long>(meta.size);
    if (index < 0)
        index += sz;
    if (index < 0 || index >= sz)
        return std::nullopt;

    if (std::holds_alternative<std::deque<std::string>>(meta.embeded_data))
    {
        auto &dq = std::get<std::deque<std::string>>(meta.embeded_data);
        return dq[index];
    }
    else
    {
        // 1. 获取首块，获取其实际大小
        std::string head_key = KeyEncoder::encode_list_sub_key(key, meta.version, meta.head_seq);
        std::optional<EValue> head_val = storage->get_raw(head_key);
        if (!head_val.has_value() || head_val->is_deleted())
            return std::nullopt;

        ListElement head_chunk = std::get<ListElement>(head_val->value);
        long long S_h = head_chunk.values.size();

        // 2. 如果整个队列只有一个块，或者索引命中在首块内，直接返回
        if (meta.head_seq == meta.tail_seq || index < S_h)
        {
            return head_chunk.values[index];
        }

        // 3. O(1) 核心跳跃公式：减去首块大小后，进行除法和取模定位
        long long rem = index - S_h;
        uint64_t target_seq = meta.head_seq + 1 + (rem / FIXED_DEQUE_CHUNK_SIZE);
        long long target_idx = rem % FIXED_DEQUE_CHUNK_SIZE;

        // 4. 精准点查目标块！
        std::string target_key = KeyEncoder::encode_list_sub_key(key, meta.version, target_seq);
        std::optional<EValue> target_val = storage->get_raw(target_key);
        if (target_val.has_value() && !target_val->is_deleted())
        {
            ListElement target_chunk = std::get<ListElement>(target_val->value);
            if (target_idx < target_chunk.values.size())
            {
                return target_chunk.values[target_idx];
            }
        }
    }
    return std::nullopt;
}
size_t DequeProcessor::l_size(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!deque_read_meta(storage, key, meta, meta_val))
        return 0;
    return meta.size;
}
std::vector<std::string> DequeProcessor::l_pop_n(Storage *storage, const std::string_view key, size_t n, const bool is_recover)
{
    return l_pop_n(storage, std::string(key), n, is_recover);
}
std::vector<std::string> DequeProcessor::r_pop_n(Storage *storage, const std::string_view key, size_t n, const bool is_recover)
{
    return r_pop_n(storage, std::string(key), n, is_recover);
}

// HashProcessor
std::vector<uint8_t> HashProcessor::get_supported_types() const
{
    return {OperationType::kHSet, OperationType::kHGet, OperationType::kHDel, OperationType::kHKeys, OperationType::kHValues, OperationType::kHEntries};
}

Response HashProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (args.empty())
        return Response::error("missing key");
    std::string key = args[0];

    switch (type)
    {
    case OperationType::kHSet:
        if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            return Response::error("wrong number of arguments for 'hset'");
        {
            std::vector<std::pair<std::string, std::string>> field_values;
            for (size_t i = 1; i < args.size(); i += 2)
                field_values.emplace_back(args[i], args[i + 1]);
            return Response::success(std::to_string(h_set(storage, key, field_values)));
        }
    case OperationType::kHGet:
        if (args.size() < 2)
            return Response::error("missing field");
        {
            auto v = h_get(storage, key, args[1]);
            return v.has_value() ? Response::success(v.value()) : Response::error("not found");
        }
    case OperationType::kHDel:
        if (args.size() < 2)
            return Response::error("missing field");
        {
            std::vector<std::string> fields;
            for (size_t i = 1; i < args.size(); ++i)
                fields.push_back(args[i]);
            return Response::success(std::to_string(h_del(storage, key, fields)));
        }
    case OperationType::kHKeys:
        return Response::success(h_keys(storage, key));
    case OperationType::kHValues:
        return Response::success(h_values(storage, key));
    case OperationType::kHEntries:
    {
        auto m = h_entries(storage, key);
        std::vector<std::pair<std::string, EyaValue>> vec;
        for (auto &p : m)
            vec.push_back({p.first, p.second});
        return Response::success(vec);
    }
    default:
        return Response::error("unsupported type");
    }
}

Response HashProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string_view> &args)
{
    std::vector<std::string> str_args;
    for (auto v : args)
        str_args.emplace_back(v);
    return execute(storage, type, str_args);
}

bool HashProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    if (type == OperationType::kHSet)
    {
        std::vector<std::pair<std::string, std::string>> field_values;
        while (offset < payload.size())
        {
            std::string field = Serializer::deserializeString(payload.data(), offset);
            std::string value = Serializer::deserializeString(payload.data(), offset);
            field_values.emplace_back(field, value);
        }
        h_set(storage, key, field_values, true);
    }
    else if (type == OperationType::kHDel)
    {
        std::vector<std::string> fields;
        while (offset < payload.size())
            fields.push_back(Serializer::deserializeString(payload.data(), offset));
        h_del(storage, key, fields, true);
    }
    else
        return false;
    return true;
}

bool HashProcessor::hash_read_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kHash);
}

void HashProcessor::hash_get_or_create_meta(Storage *storage, const std::string &key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a hash");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kHash))
            throw std::runtime_error("value is not a hash");
    }

    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kHash);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = std::vector<std::pair<std::string, std::string>>(); // 初始使用 embeded_data 存储小哈希
    }
}

bool HashProcessor::hash_read_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val)
{
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
        return false;
    if (!std::holds_alternative<Metadata>(meta_val->value))
        return false;
    meta = std::get<Metadata>(meta_val->value);
    return meta.type == static_cast<uint8_t>(EyaType::kHash);
}

void HashProcessor::hash_get_or_create_meta(Storage *storage, std::string_view key, Metadata &meta, std::optional<EValue> &meta_val, bool &is_new)
{
    is_new = false;
    meta_val = storage->get_raw(key);
    if (!meta_val.has_value() || meta_val->is_deleted() || meta_val->is_expired())
    {
        is_new = true;
    }
    else if (!std::holds_alternative<Metadata>(meta_val->value))
    {
        throw std::runtime_error("value is not a hash");
    }
    else
    {
        meta = std::get<Metadata>(meta_val->value);
        if (meta.type != static_cast<uint8_t>(EyaType::kHash))
            throw std::runtime_error("value is not a hash");
    }

    if (is_new)
    {
        meta.type = static_cast<uint8_t>(EyaType::kHash);
        meta.version = Metadata::generate_version();
        meta.size = 0;
        meta.embeded_data = std::vector<std::pair<std::string, std::string>>(); // 初始使用 embeded_data 存储小哈希
    }
}

size_t HashProcessor::h_set(Storage *storage, const std::string &key, const std::vector<std::pair<std::string, std::string>> &field_values, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &kv : field_values)
            payload += Serializer::serialize(kv.first) + Serializer::serialize(kv.second);
        storage->wal_->append_log(OperationType::kHSet, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    bool is_new = false;
    hash_get_or_create_meta(storage, key, meta, meta_val, is_new);
    std::vector<std::pair<std::string, EValue>> batch;
    size_t new_fields = 0;

    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
        for (const auto &kv : field_values)
        {
            auto it = std::find_if(vec.begin(), vec.end(), [&](const auto &p)
                                   { return p.first == kv.first; });
            if (it != vec.end())
            {
                it->second = kv.second; // 覆盖旧值
            }
            else
            {
                vec.emplace_back(kv.first, kv.second); // 插入新值
                new_fields++;
            }
        }
        meta.size = vec.size();

        if (vec.size() >= MAX_INLINE_SIZE)
        {
            for (const auto &kv : vec)
            {
                std::string sub_key = KeyEncoder::encode_hash_sub_key(key, meta.version, kv.first);
                EValue sub_val;
                sub_val.value = kv.second;
                // storage->write_memtable(sub_key, sub_val);
                batch.emplace_back(sub_key, sub_val);
            }
            meta.embeded_data = std::monostate();
        }
    }
    else
    {
        for (const auto &kv : field_values)
        {
            std::string sub_key = KeyEncoder::encode_hash_sub_key(key, meta.version, kv.first);
            std::optional<EValue> existing_val = storage->get_raw(sub_key);
            if (!existing_val.has_value() || existing_val->is_deleted())
                new_fields++;

            EValue sub_val;
            sub_val.value = kv.second;
            // storage->write_memtable(sub_key, sub_val);
            batch.emplace_back(sub_key, sub_val);
        }
        meta.size += new_fields;
    }

    EValue updated_meta;
    updated_meta.value = meta;
    if (!is_new && meta_val.has_value())
        updated_meta.expire_time = meta_val->expire_time;
    // storage->write_memtable(key, updated_meta);
    batch.emplace_back(key, updated_meta);
    storage->write_batch(batch);
    return new_fields;
}

std::optional<std::string> HashProcessor::h_get(Storage *storage, const std::string &key, const std::string &field)
{
    return h_get(storage, std::string_view(key), std::string_view(field));
}

size_t HashProcessor::h_del(Storage *storage, const std::string &key, const std::vector<std::string> &fields, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload;
        for (const auto &f : fields)
            payload += Serializer::serialize(f);
        storage->wal_->append_log(OperationType::kHDel, key, payload);
    }

    Metadata meta;
    std::optional<EValue> meta_val;
    if (!hash_read_meta(storage, key, meta, meta_val))
        return 0;
    std::vector<std::pair<std::string, EValue>> batch;
    size_t deleted_count = 0;

    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
        for (const auto &f : fields)
        {
            auto it = std::find_if(vec.begin(), vec.end(), [&](const auto &p)
                                   { return p.first == f; });
            if (it != vec.end())
            {
                vec.erase(it);
                deleted_count++;
            }
        }
        meta.size = vec.size();
    }
    else
    {
        for (const auto &f : fields)
        {
            std::string sub_key = KeyEncoder::encode_hash_sub_key(key, meta.version, f);
            std::optional<EValue> sub_val_opt = storage->get_raw(sub_key);
            if (sub_val_opt.has_value() && !sub_val_opt->is_deleted())
            {
                EValue del_val;
                del_val.deleted = true;
                // storage->write_memtable(sub_key, del_val);
                batch.emplace_back(sub_key, del_val);
                deleted_count++;
            }
        }
        meta.size -= deleted_count;
    }

    if (deleted_count > 0)
    {
        EValue updated_meta;
        updated_meta.value = meta;
        updated_meta.expire_time = meta_val->expire_time;
        // storage->write_memtable(key, updated_meta);
        batch.emplace_back(key, updated_meta);
        storage->write_batch(batch);
    }
    return deleted_count;
}

std::vector<std::string> HashProcessor::h_keys(Storage *storage, const std::string &key)
{
    return h_keys(storage, std::string_view(key));
}

std::vector<std::string> HashProcessor::h_values(Storage *storage, const std::string &key)
{
    return h_values(storage, std::string_view(key));
}

std::vector<std::pair<std::string, std::string>> HashProcessor::h_entries(Storage *storage, const std::string &key)
{
    return h_entries(storage, std::string_view(key));
}

// string_view delegate methods
size_t HashProcessor::h_set(Storage *storage, const std::string_view key, const std::vector<std::pair<std::string_view, std::string_view>> &field_values, const bool is_recover)
{
    std::vector<std::pair<std::string, std::string>> str_fv;
    for (auto &kv : field_values)
        str_fv.emplace_back(std::string(kv.first), std::string(kv.second));
    return h_set(storage, std::string(key), str_fv, is_recover);
}

std::optional<std::string> HashProcessor::h_get(Storage *storage, const std::string_view key, const std::string_view field)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!hash_read_meta(storage, key, meta, meta_val))
        return std::nullopt;

    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
        auto it = std::find_if(vec.begin(), vec.end(), [&](const auto &p)
                               { return p.first == field; });
        if (it != vec.end())
            return it->second;
    }
    else
    {
        std::string sub_key = KeyEncoder::encode_hash_sub_key(key, meta.version, field);
        std::optional<EValue> sub_val_opt = storage->get_raw(sub_key);
        if (sub_val_opt.has_value() && !sub_val_opt->is_deleted() && std::holds_alternative<std::string>(sub_val_opt->value))
            return std::get<std::string>(sub_val_opt->value);
    }
    return std::nullopt;
}

size_t HashProcessor::h_del(Storage *storage, const std::string_view key, const std::vector<std::string_view> &fields, const bool is_recover)
{
    std::vector<std::string> str_fields;
    for (auto &f : fields)
        str_fields.emplace_back(f);
    return h_del(storage, std::string(key), str_fields, is_recover);
}

std::vector<std::string> HashProcessor::h_keys(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!hash_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::string> result;
    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
        for (const auto &kv : vec)
            result.push_back(kv.first);
    }
    else
    {
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kHash, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);
        for (const auto &pair : kv_pairs)
            result.emplace_back(KeyEncoder::decode_hash_field(pair.first));
    }
    return result;
}
std::vector<std::string> HashProcessor::h_values(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!hash_read_meta(storage, key, meta, meta_val))
        return {};

    std::vector<std::string> result;
    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        auto &vec = std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
        for (const auto &kv : vec)
            result.push_back(kv.second);
    }
    else
    {
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kHash, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);
        for (const auto &pair : kv_pairs)
        {
            if (std::holds_alternative<std::string>(pair.second))
                result.emplace_back(std::get<std::string>(pair.second));
        }
    }
    return result;
}
std::vector<std::pair<std::string, std::string>> HashProcessor::h_entries(Storage *storage, const std::string_view key)
{
    Metadata meta;
    std::optional<EValue> meta_val;
    if (!hash_read_meta(storage, key, meta, meta_val))
        return {};

    if (std::holds_alternative<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data))
    {
        return std::get<std::vector<std::pair<std::string, std::string>>>(meta.embeded_data);
    }
    else
    {
        std::vector<std::pair<std::string, std::string>> result;
        std::string prefix = KeyEncoder::get_complex_prefix(ColumnFamily::kHash, key, meta.version);
        std::string end_prefix = KeyEncoder::get_prefix_end(prefix);
        auto kv_pairs = storage->range(prefix, end_prefix);
        for (const auto &pair : kv_pairs)
        {
            std::string field(KeyEncoder::decode_hash_field(pair.first));
            if (std::holds_alternative<std::string>(pair.second))
                result.emplace_back(field, std::get<std::string>(pair.second));
        }
        return result;
    }
}