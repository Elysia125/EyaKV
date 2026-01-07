#include "storage/processors/structure_processors.h"
#include "logger/logger.h"
#include "storage/storage.h"
#include "common/serialization/serializer.h"
#include "common/types/operation_type.h"
#include <limits>

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
        storage->wal_->append_log(OperationType::kSet, key, serialize(val));
    }
    storage->write_memtable(key, val);
    return true;
}

Response StringProcessor::execute(Storage *storage, const uint8_t type, const std::vector<std::string> &args)
{
    if (type == OperationType::kSet)
    {
        if (args.size() < 2)
        {
            return Response::error("missing arguments");
        }
        return Response::success(set(storage, args[0], args[1], args.size() > 2 ? std::stoll(args[2]) : 0));
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
    {
        return Response::error("missing key");
    }
    std::string key = args[0];

    switch (type)
    {
    case OperationType::kSAdd:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        size_t added = 0;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (s_add(storage, key, args[i]))
            {
                added++;
            }
        }
        return Response::success(added);
    }
    case OperationType::kSRem:
    {
        if (args.size() < 2)
            return Response::error("missing member");
        size_t removed = 0;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (s_rem(storage, key, args[i]))
            {
                removed++;
            }
        }
        return Response::success(removed);
    }
    case OperationType::kSMembers:
    {
        return Response::success(s_members(storage, key));
    }
    default:
        return Response::error("unsupported type");
    }
}

bool SetProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    std::string member = Serializer::deserializeString(payload.data(), offset);

    if (type == OperationType::kSAdd)
    {
        return s_add(storage, key, member, true);
    }
    else if (type == OperationType::kSRem)
    {
        return s_rem(storage, key, member, true);
    }
    return false;
}

bool SetProcessor::s_add(Storage *storage, const std::string &key, const std::string &member, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kSAdd, key, Serializer::serialize(member));
    }
    try
    {
        storage->memtable_->handle_value(key, [&member](EValue &val) -> EValue &
                                         {
                                            if (!std::holds_alternative<std::unordered_set<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a set");
                                            }
                                            auto &set = std::get<std::unordered_set<std::string>>(val.value);
                                            if (val.is_deleted() || val.is_expired())
                                            {
                                                val.deleted = false;
                                                val.expire_time = 0;
                                                set.clear();
                                            }
                                            set.insert(member);
                                            return val; });
        return true;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::unordered_set<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a set");
            }
            auto &set = std::get<std::unordered_set<std::string>>(val.value);
            set.insert(member);
            storage->write_memtable(key, val);
        }
        else
        {
            std::unordered_set<std::string> set;
            set.insert(member);
            EValue val(set);
            storage->write_memtable(key, val);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("s_add key: %s, member: %s, error: %s", key, member, e.what());
        throw e;
    }
}

bool SetProcessor::s_rem(Storage *storage, const std::string &key, const std::string &member, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kSRem, key, Serializer::serialize(member));
    }
    try
    {
        storage->memtable_->handle_value(key, [&member](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted()||val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<std::unordered_set<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a set");
                                            }
                                            else
                                            {
                                                auto &set = std::get<std::unordered_set<std::string>>(val.value);
                                                set.erase(member);
                                                return val;
                                            } });
        return true;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::unordered_set<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a set");
            }
            auto &set = std::get<std::unordered_set<std::string>>(val.value);
            set.erase(member);
            storage->write_memtable(key, val);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("s_add key: %s, member: %s, error: %s", key, member, e.what());
        throw e;
    }
}

std::vector<std::string> SetProcessor::s_members(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
    {
        return {};
    }

    if (!std::holds_alternative<std::unordered_set<std::string>>(val_opt.value()))
    {
        throw std::runtime_error("value is not a set");
    }
    auto &set = std::get<std::unordered_set<std::string>>(val_opt.value());
    return std::vector<std::string>(set.begin(), set.end());
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
        if (args.size() < 3)
            return Response::error("missing score or member");
        return Response::success(z_add(storage, key, args[1], args[2]));
    case OperationType::kZRem:
        if (args.size() < 2)
            return Response::error("missing member");
        return Response::success(z_rem(storage, key, args[1]));
    case OperationType::kZScore:
        if (args.size() < 2)
            return Response::error("missing member");
        {
            auto score = z_score(storage, key, args[1]);
            return score.has_value() ? Response::success(score.value()) : Response::error("not found"); // Or null
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
        // args: key, start, end
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

bool ZSetProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    switch (type)
    {
    case OperationType::kZAdd:
    {
        auto score = Serializer::deserializeString(payload.data(), offset);
        auto member = Serializer::deserializeString(payload.data(), offset);
        z_add(storage, key, score, member, true);
        break;
    }
    case OperationType::kZRem:
    {
        auto member = Serializer::deserializeString(payload.data(), offset);
        z_rem(storage, key, member, true);
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

// ZSet Helpers
bool ZSetProcessor::z_add(Storage *storage, const std::string &key, const std::string &score, const std::string &member, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(score) + Serializer::serialize(member);
        storage->wal_->append_log(OperationType::kZAdd, key, payload);
    }
    try
    {
        storage->memtable_->handle_value(key, [&score, &member](EValue &val) -> EValue &
                                         {
                                            if (!std::holds_alternative<ZSet>(val.value))
                                            {
                                                throw std::runtime_error("value is not a zset");
                                            }
                                            auto &zset = std::get<ZSet>(val.value);
                                            if(val.is_deleted()||val.is_expired()){
                                                val.deleted = false;
                                                val.expire_time = 0;
                                                zset.z_clear();
                                            }
                                            zset.zadd(member, score);
                                                return val; });
        return true;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<ZSet>(val.value))
            {
                throw std::runtime_error("value is not a zset");
            }
            auto &zset = std::get<ZSet>(val.value);
            zset.zadd(member, score);
            storage->write_memtable(key, val);
            return true;
        }
        else
        {
            ZSet zset;
            zset.zadd(member, score);
            EValue val;
            val.value = zset;
            storage->write_memtable(key, val);
            return true;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("z_add key: %s, score: %s, member: %s, error: %s", key, score, member, e.what());
        throw e;
    }
    return false;
}

bool ZSetProcessor::z_rem(Storage *storage, const std::string &key, const std::string &member, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(member);
        storage->wal_->append_log(OperationType::kZRem, key, payload);
    }
    try
    {
        storage->memtable_->handle_value(key, [&member](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted()||val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<ZSet>(val.value))
                                            {
                                                throw std::runtime_error("value is not a zset");
                                            }
                                            auto &zset = std::get<ZSet>(val.value);
                                            zset.zrem(member);
                                            return val; });
        return true;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<ZSet>(val.value))
            {
                throw std::runtime_error("value is not a zset");
            }
            auto &zset = std::get<ZSet>(val.value);
            zset.zrem(member);
            storage->write_memtable(key, val);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("z_rem key: %s, member: %s, error: %s", key, member, e.what());
        throw e;
    }
    return false;
}

std::optional<std::string> ZSetProcessor::z_score(Storage *storage, const std::string &key, const std::string &member)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return std::nullopt;
    if (!std::holds_alternative<ZSet>(val_opt.value()))
        throw std::runtime_error("value is not a zset");
    return std::get<ZSet>(val_opt.value()).zscore(member);
}

std::optional<size_t> ZSetProcessor::z_rank(Storage *storage, const std::string &key, const std::string &member)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return std::nullopt;
    if (!std::holds_alternative<ZSet>(val_opt.value()))
        throw std::runtime_error("value is not a zset");
    return std::get<ZSet>(val_opt.value()).zrank(member);
}

size_t ZSetProcessor::z_card(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return 0;
    if (!std::holds_alternative<ZSet>(val_opt.value()))
        throw std::runtime_error("value is not a zset");
    return std::get<ZSet>(val_opt.value()).zcard();
}

std::string ZSetProcessor::z_incr_by(Storage *storage, const std::string &key, const std::string &increment, const std::string &member, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(increment) + Serializer::serialize(member);
        storage->wal_->append_log(OperationType::kZIncrBy, key, payload);
    }
    std::optional<std::string> new_score;
    try
    {
        storage->memtable_->handle_value(key, [&increment, &member, &new_score](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted()||val.is_expired()){
                                                throw std::runtime_error("key not be found");
                                            }
                                            if (!std::holds_alternative<ZSet>(val.value))
                                            {
                                                throw std::runtime_error("value is not a zset");
                                            }
                                            else
                                            {
                                                auto &zset = std::get<ZSet>(val.value);
                                                new_score = zset.z_incrby(member, increment);
                                                return val;
                                            } });
        return new_score == std::nullopt ? std::string("Member not be found") : new_score.value();
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<ZSet>(val.value))
            {
                throw std::runtime_error("value is not a zset");
            }
            auto &zset = std::get<ZSet>(val.value);
            new_score = zset.z_incrby(member, increment);
            storage->write_memtable(key, val);
            return new_score == std::nullopt ? std::string("Member not be found") : new_score.value();
        }
        else
        {
            throw std::runtime_error("key not be found");
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("z_incr_by key: %s, increment: %s, member: %s, error: %s", key, increment, member, e.what());
        throw e;
    }
    return "";
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_rank(Storage *storage, const std::string &key, long long start, long long end)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<ZSet>(val_opt.value()))
        throw std::runtime_error("value is not a zset");
    auto &zset = std::get<ZSet>(val_opt.value());

    long long size = zset.zcard();
    if (start < 0)
        start += size;
    if (end < 0)
        end += size;
    if (start < 0)
        start = 0;
    if (end >= size)
        end = size - 1;
    if (start > end)
        return {};

    auto res = zset.zrange_by_rank((size_t)start, (size_t)end);
    std::vector<std::pair<std::string, EyaValue>> ret;
    for (auto &p : res)
    {
        ret.push_back({p.first, p.second});
    }
    return ret;
}

std::vector<std::pair<std::string, EyaValue>> ZSetProcessor::z_range_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<ZSet>(val_opt.value()))
        throw std::runtime_error("value is not a zset");
    auto &zset = std::get<ZSet>(val_opt.value());

    auto res = zset.zrange_by_score(min, max);
    std::vector<std::pair<std::string, EyaValue>> ret;
    for (auto &p : res)
    {
        ret.push_back({p.first, p.second});
    }
    return ret;
}

size_t ZSetProcessor::z_rem_by_rank(Storage *storage, const std::string &key, long long start, long long end, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(std::to_string(start)) + Serializer::serialize(std::to_string(end));
        storage->wal_->append_log(OperationType::kZRemByRank, key, payload);
    }
    size_t count = 0;
    try
    {
        storage->memtable_->handle_value(key, [&start, &end, &count](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted()||val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<ZSet>(val.value))
                                            {
                                                throw std::runtime_error("value is not a zset");
                                            }
                                            else
                                            {
                                                auto &zset = std::get<ZSet>(val.value);
                                                long long size = zset.zcard();
                                                long long s = start, e = end;
                                                if (s < 0) s += size;
                                                if (e < 0) e += size;
                                                if (s < 0) s = 0;
                                                if (e >= size) e = size - 1;
                                                if (s > e) count = 0;
                                                else count = zset.zrem_range_by_rank((size_t)s, (size_t)e);
                                                return val;
                                            } });
        return count;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<ZSet>(val.value))
            {
                throw std::runtime_error("value is not a zset");
            }
            auto &zset = std::get<ZSet>(val.value);
            long long size = zset.zcard();
            long long s = start, e = end;
            if (s < 0)
                s += size;
            if (e < 0)
                e += size;
            if (s < 0)
                s = 0;
            if (e >= size)
                e = size - 1;
            if (s > e)
                count = 0;
            else
            {
                count = zset.zrem_range_by_rank((size_t)s, (size_t)e);
                storage->write_memtable(key, val);
            }
            return count;
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("z_rem_by_rank key: %s, start: %lld, end: %lld, error: %s", key, start, end, e.what());
        throw e;
    }
    return 0;
}

size_t ZSetProcessor::z_rem_by_score(Storage *storage, const std::string &key, const std::string &min, const std::string &max, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(min) + Serializer::serialize(max);
        storage->wal_->append_log(OperationType::kZRemByScore, key, payload);
    }
    size_t count = 0;
    try
    {
        storage->memtable_->handle_value(key, [&min, &max, &count](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted()||val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<ZSet>(val.value))
                                            {
                                                throw std::runtime_error("value is not a zset");
                                            }
                                            else
                                            {
                                                auto &zset = std::get<ZSet>(val.value);
                                                count = zset.zrem_range_by_score(min, max);
                                                return val;
                                            } });
        return count;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            auto &zset = std::get<ZSet>(val.value);
            count = zset.zrem_range_by_score(min, max);
            storage->write_memtable(key, val);
        }
        return count;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("z_rem_by_score key: %s, min: %s, max: %s, error: %s", key, min, max, e.what());
        throw e;
    }
    return 0;
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
        if (args.size() < 2)
            return Response::error("missing value");
        {
            std::vector<std::string> values;
            for (size_t i = 1; i < args.size(); ++i)
                values.push_back(args[i]);
            return Response::success(std::to_string(l_push(storage, key, values)));
        }
    case OperationType::kRPush:
        if (args.size() < 2)
            return Response::error("missing value");
        {
            std::vector<std::string> values;
            for (size_t i = 1; i < args.size(); ++i)
                values.push_back(args[i]);
            return Response::success(std::to_string(r_push(storage, key, values)));
        }
    case OperationType::kLPop:
    {
        if (args.size() == 1)
        {
            auto v = l_pop(storage, key);
            return v.has_value() ? Response::success(v.value()) : Response::error("empty");
        }
        else
        {
            size_t count = std::stoull(args[1]);
            return Response::success(l_pop_n(storage, key, count));
        }
    }
    case OperationType::kRPop:
    {
        if (args.size() == 1)
        {
            auto v = r_pop(storage, key);
            return v.has_value() ? Response::success(v.value()) : Response::error("empty");
        }
        else
        {
            size_t count = std::stoull(args[1]);
            return Response::success(r_pop_n(storage, key, count));
        }
    }
    case OperationType::kLSize:
        return Response::success(std::to_string(l_size(storage, key)));
    case OperationType::kLRange:
        if (args.size() < 3)
            return Response::error("missing start/end");
        {
            auto vec = l_range(storage, key, std::stoll(args[1]), std::stoll(args[2]));
            return Response::success(vec);
        }
    case OperationType::kLGet:
        if (args.size() < 2)
            return Response::error("missing index");
        {
            auto v = l_get(storage, key, std::stoll(args[1]));
            return v.has_value() ? Response::success(v.value()) : Response::error("not found");
        }
    case OperationType::kLPopN:
        if (args.size() < 2)
            return Response::error("missing count");
        return Response::success(l_pop_n(storage, key, std::stoll(args[1])));
    case OperationType::kRPopN:
        if (args.size() < 2)
            return Response::error("missing count");
        return Response::success(r_pop_n(storage, key, std::stoll(args[1])));
    default:
        return Response::error("unsupported type");
    }
}

bool DequeProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    if (type == OperationType::kLPush)
    {
        std::vector<std::string> values;
        Serializer::deserializeVector(payload.data(), offset, values);
        l_push(storage, key, values, true);
    }
    else if (type == OperationType::kRPush)
    {
        std::vector<std::string> values;
        Serializer::deserializeVector(payload.data(), offset, values);
        r_push(storage, key, values, true);
    }
    else if (type == OperationType::kLPop)
    {
        l_pop(storage, key, true);
    }
    else if (type == OperationType::kRPop)
    {
        r_pop(storage, key, true);
    }
    else if (type == OperationType::kLPopN)
    {
        std::string count_str = Serializer::deserializeString(payload.data(), offset);
        l_pop_n(storage, key, std::stoll(count_str), true);
    }
    else if (type == OperationType::kRPopN)
    {
        std::string count_str = Serializer::deserializeString(payload.data(), offset);
        r_pop_n(storage, key, std::stoll(count_str), true);
    }
    else
    {
        return false;
    }
    return true;
}

size_t DequeProcessor::l_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kLPush, key, Serializer::serialize(values));
    }
    size_t size = 0;
    try
    {
        storage->memtable_->handle_value(key, [&values, &size](EValue &val) -> EValue &
                                         {
                                            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a list");
                                            }
                                            auto &dq = std::get<std::deque<std::string>>(val.value);
                                            if(val.is_deleted() || val.is_expired()){
                                                val.deleted=false;
                                                val.expire_time=0;
                                                dq.clear();
                                            }
                                            for(const auto& value : values) {
                                                dq.push_front(value);
                                            }
                                            size = dq.size();
                                            return val; });
        return size;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (const auto &value : values)
            {
                dq.push_front(value);
            }
            size = dq.size();
            storage->write_memtable(key, val);
            return size;
        }
        else
        {
            std::deque<std::string> dq;
            for (const auto &value : values)
            {
                dq.push_front(value);
            }
            size = dq.size();
            EValue val;
            val.value = dq;
            storage->write_memtable(key, val);
            return size;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("l_push key: %s, error: %s", key, e.what());
        throw e;
    }
}

size_t DequeProcessor::r_push(Storage *storage, const std::string &key, const std::vector<std::string> &values, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kRPush, key, Serializer::serialize(values));
    }
    size_t size = 0;
    try
    {
        storage->memtable_->handle_value(key, [&values, &size](EValue &val) -> EValue &
                                         {
                                            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a list");
                                            }
                                            auto &dq = std::get<std::deque<std::string>>(val.value);
                                             if(val.is_deleted() || val.is_expired()){
                                                val.deleted=false;
                                                val.expire_time=0;
                                                dq.clear();
                                            }
                                            for(const auto& value : values) {
                                                dq.push_back(value);
                                            }
                                            size = dq.size();
                                            return val; });
        return size;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (const auto &value : values)
            {
                dq.push_back(value);
            }
            size = dq.size();
            storage->write_memtable(key, val);
            return size;
        }
        else
        {
            std::deque<std::string> dq;
            for (const auto &value : values)
            {
                dq.push_back(value);
            }
            size = dq.size();
            EValue val;
            val.value = dq;
            storage->write_memtable(key, val);
            return size;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("r_push key: %s, error: %s", key, e.what());
        throw e;
    }
}

std::optional<std::string> DequeProcessor::l_pop(Storage *storage, const std::string &key, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kLPop, key, "");
    }
    std::optional<std::string> popped_val = std::nullopt;
    try
    {
        storage->memtable_->handle_value(key, [&popped_val](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted() || val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a list");
                                            }
                                            auto &dq = std::get<std::deque<std::string>>(val.value);
                                            if (!dq.empty()) {
                                                popped_val = dq.front();
                                                dq.pop_front();
                                            }
                                            return val; });
        return popped_val;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            if (!dq.empty())
            {
                popped_val = dq.front();
                dq.pop_front();
                storage->write_memtable(key, val);
            }
            return popped_val;
        }
        else
        {
            return std::nullopt;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("l_pop key: %s, error: %s", key, e.what());
        throw e;
    }
}

std::optional<std::string> DequeProcessor::r_pop(Storage *storage, const std::string &key, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kRPop, key, "");
    }
    std::optional<std::string> popped_val = std::nullopt;
    try
    {
        storage->memtable_->handle_value(key, [&popped_val](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted() || val.is_expired()){
                                                return val;
                                            }
                                            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a list");
                                            }
                                            auto &dq = std::get<std::deque<std::string>>(val.value);
                                            if (!dq.empty()) {
                                                popped_val = dq.back();
                                                dq.pop_back();
                                            }
                                            return val; });
        return popped_val;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
            {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            if (!dq.empty())
            {
                popped_val = dq.back();
                dq.pop_back();
                storage->write_memtable(key, val);
            }
            return popped_val;
        }
        else
        {
            return std::nullopt;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("r_pop key: %s, error: %s", key, e.what());
        throw e;
    }
}

std::vector<std::string> DequeProcessor::l_range(Storage *storage, const std::string &key, long long start, long long end)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<std::deque<std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a list");
    auto &dq = std::get<std::deque<std::string>>(val_opt.value());

    if (start < 0)
        start += dq.size();
    if (end < 0)
        end += dq.size();
    if (start < 0)
        start = 0;
    if (end >= (long long)dq.size())
        end = dq.size() - 1;

    std::vector<std::string> res;
    if (start > end)
        return res;

    for (long long i = start; i <= end; ++i)
    {
        res.push_back(dq[i]);
    }
    return res;
}

std::optional<std::string> DequeProcessor::l_get(Storage *storage, const std::string &key, long long index)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return std::nullopt;
    if (!std::holds_alternative<std::deque<std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a list");
    auto &dq = std::get<std::deque<std::string>>(val_opt.value());

    if (index < 0)
        index += dq.size();
    if (index < 0 || index >= (long long)dq.size())
        return std::nullopt;
    return dq[index];
}

size_t DequeProcessor::l_size(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return 0;
    if (!std::holds_alternative<std::deque<std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a list");
    return std::get<std::deque<std::string>>(val_opt.value()).size();
}

std::vector<std::string> DequeProcessor::l_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kLPopN, key, Serializer::serialize(std::to_string(n)));
    }
    std::vector<std::string> popped;
    try
    {
        storage->memtable_->handle_value(key, [&](EValue &val) -> EValue &
                                         {
            if(val.is_deleted() || val.is_expired()){
                return val;
            }
            if (!std::holds_alternative<std::deque<std::string>>(val.value)) {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (size_t i = 0; i < n && !dq.empty(); ++i) {
                popped.push_back(dq.front());
                dq.pop_front();
            }
            return val; });
        return popped;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                throw std::runtime_error("value is not a list");
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (size_t i = 0; i < n && !dq.empty(); ++i)
            {
                popped.push_back(dq.front());
                dq.pop_front();
            }
            storage->write_memtable(key, val);
            return popped;
        }
        else
        {
            return {};
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("l_pop_n key: %s, error: %s", key, e.what());
        throw e;
    }
}

std::vector<std::string> DequeProcessor::r_pop_n(Storage *storage, const std::string &key, size_t n, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kRPopN, key, Serializer::serialize(std::to_string(n)));
    }
    std::vector<std::string> popped;
    try
    {
        storage->memtable_->handle_value(key, [&](EValue &val) -> EValue &
                                         {
            if(val.is_deleted() || val.is_expired()){
                return val;
            }
            if (!std::holds_alternative<std::deque<std::string>>(val.value)) {
                throw std::runtime_error("value is not a list");
            }
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (size_t i = 0; i < n && !dq.empty(); ++i) {
                popped.push_back(dq.back());
                dq.pop_back();
            }
            return val; });
        return popped;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::deque<std::string>>(val.value))
                throw std::runtime_error("value is not a list");
            auto &dq = std::get<std::deque<std::string>>(val.value);
            for (size_t i = 0; i < n && !dq.empty(); ++i)
            {
                popped.push_back(dq.back());
                dq.pop_back();
            }
            storage->write_memtable(key, val);
            return popped;
        }
        else
        {
            return {};
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("r_pop_n key: %s, error: %s", key, e.what());
        throw e;
    }
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
        if (args.size() < 3)
            return Response::error("missing field/value");
        return Response::success(h_set(storage, key, args[1], args[2]) ? "1" : "0");
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
        return Response::success(h_del(storage, key, args[1]) ? "1" : "0");
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

bool HashProcessor::recover(Storage *storage, const uint8_t type, const std::string &key, const std::string &payload)
{
    size_t offset = 0;
    if (type == OperationType::kHSet)
    {
        std::string field = Serializer::deserializeString(payload.data(), offset);
        std::string value = Serializer::deserializeString(payload.data(), offset);
        h_set(storage, key, field, value, true);
    }
    else if (type == OperationType::kHDel)
    {
        std::string field = Serializer::deserializeString(payload.data(), offset);
        h_del(storage, key, field, true);
    }
    else
    {
        return false;
    }
    return true;
}

bool HashProcessor::h_set(Storage *storage, const std::string &key, const std::string &field, const std::string &value, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        std::string payload = Serializer::serialize(field) + Serializer::serialize(value);
        storage->wal_->append_log(OperationType::kHSet, key, payload);
    }
    bool is_new = false;
    try
    {
        storage->memtable_->handle_value(key, [&field, &value, &is_new](EValue &val) -> EValue &
                                         {
                                            if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a hash");
                                            }
                                            auto &map = std::get<std::unordered_map<std::string, std::string>>(val.value);
                                            if(val.is_deleted() || val.is_expired()){
                                                val.deleted=false;
                                                val.expire_time=0;
                                                map.clear();
                                            }
                                            if (map.find(field) == map.end()) is_new = true;
                                            map[field] = value;
                                            return val; });
        return is_new;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.value))
            {
                throw std::runtime_error("value is not a hash");
            }
            auto &map = std::get<std::unordered_map<std::string, std::string>>(val.value);
            if (map.find(field) == map.end())
                is_new = true;
            map[field] = value;
            storage->write_memtable(key, val);
            return is_new;
        }
        else
        {
            std::unordered_map<std::string, std::string> map;
            map[field] = value;
            EValue val;
            val.value = map;
            storage->write_memtable(key, val);
            return true;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("h_set key: %s, field: %s, error: %s", key, field, e.what());
        throw e;
    }
}

std::optional<std::string> HashProcessor::h_get(Storage *storage, const std::string &key, const std::string &field)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return std::nullopt;
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a hash");
    auto &map = std::get<std::unordered_map<std::string, std::string>>(val_opt.value());

    auto it = map.find(field);
    if (it != map.end())
        return it->second;
    return std::nullopt;
}

bool HashProcessor::h_del(Storage *storage, const std::string &key, const std::string &field, const bool is_recover)
{
    if (storage->enable_wal_ && storage->wal_ && !is_recover)
    {
        storage->wal_->append_log(OperationType::kHDel, key, Serializer::serialize(field));
    }
    bool deleted = false;
    try
    {
        storage->memtable_->handle_value(key, [&field, &deleted](EValue &val) -> EValue &
                                         {
                                            if(val.is_deleted() || val.is_expired()){
                                                deleted = true;
                                                return val;
                                            }
                                            if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.value))
                                            {
                                                throw std::runtime_error("value is not a hash");
                                            }
                                            auto &map = std::get<std::unordered_map<std::string, std::string>>(val.value);
                                            if(map.erase(field)) deleted = true;
                                            return val; });
        return deleted;
    }
    catch (const std::out_of_range &e)
    {
        std::optional<EValue> val_opt;
        if (storage->get_from_old(key, val_opt))
        {
            EValue val = val_opt.value();
            if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.value))
            {
                throw std::runtime_error("value is not a hash");
            }
            auto &map = std::get<std::unordered_map<std::string, std::string>>(val.value);
            if (map.erase(field))
            {
                deleted = true;
                storage->write_memtable(key, val);
            }
            return deleted;
        }
        else
        {
            return true;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("h_del key: %s, field: %s, error: %s", key, field, e.what());
        throw e;
    }
}

std::vector<std::string> HashProcessor::h_keys(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a hash");
    auto &map = std::get<std::unordered_map<std::string, std::string>>(val_opt.value());

    std::vector<std::string> res;
    for (auto &p : map)
        res.push_back(p.first);
    return res;
}

std::vector<std::string> HashProcessor::h_values(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a hash");
    auto &map = std::get<std::unordered_map<std::string, std::string>>(val_opt.value());

    std::vector<std::string> res;
    for (auto &p : map)
        res.push_back(p.second);
    return res;
}

std::unordered_map<std::string, std::string> HashProcessor::h_entries(Storage *storage, const std::string &key)
{
    auto val_opt = storage->get(key);
    if (!val_opt.has_value())
        return {};
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val_opt.value()))
        throw std::runtime_error("value is not a hash");
    return std::get<std::unordered_map<std::string, std::string>>(val_opt.value());
}
