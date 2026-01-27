#ifndef OPERATION_TYPE_H_
#define OPERATION_TYPE_H_

#include <cstdint>
#include <unordered_map>
// OperationType constants
namespace OperationType
{
    // 通用
    constexpr uint8_t kExists = 0;
    constexpr uint8_t kRemove = kExists + 1;
    constexpr uint8_t kRange = kRemove + 1;
    constexpr uint8_t kExpire = kRange + 1;
    constexpr uint8_t kGet = kExpire + 1;
    // string
    constexpr uint8_t kSet = kGet + 1;
    // set
    constexpr uint8_t kSAdd = kSet + 1;
    constexpr uint8_t kSRem = kSAdd + 1;
    constexpr uint8_t kSMembers = kSRem + 1;
    // zset
    constexpr uint8_t kZAdd = kSMembers + 1;
    constexpr uint8_t kZRem = kZAdd + 1;
    constexpr uint8_t kZScore = kZRem + 1;
    constexpr uint8_t kZRank = kZScore + 1;
    constexpr uint8_t kZCard = kZRank + 1;
    constexpr uint8_t kZIncrBy = kZCard + 1;
    constexpr uint8_t kZRangeByRank = kZIncrBy + 1;
    constexpr uint8_t kZRangeByScore = kZRangeByRank + 1;
    constexpr uint8_t kZRemByRank = kZRangeByScore + 1;
    constexpr uint8_t kZRemByScore = kZRemByRank + 1;
    // list
    constexpr uint8_t kLPush = kZRemByScore + 1;
    constexpr uint8_t kLPop = kLPush + 1;
    constexpr uint8_t kRPush = kLPop + 1;
    constexpr uint8_t kRPop = kRPush + 1;
    constexpr uint8_t kLRange = kRPop + 1;
    constexpr uint8_t kLGet = kLRange + 1;
    constexpr uint8_t kLSize = kLGet + 1;
    constexpr uint8_t kLPopN = kLSize + 1;
    constexpr uint8_t kRPopN = kLPopN + 1;
    // map
    constexpr uint8_t kHSet = kRPopN + 1;
    constexpr uint8_t kHGet = kHSet + 1;
    constexpr uint8_t kHDel = kHGet + 1;
    constexpr uint8_t kHKeys = kHDel + 1;
    constexpr uint8_t kHValues = kHKeys + 1;
    constexpr uint8_t kHEntries = kHValues + 1;
}
static std::unordered_map<std::string, uint8_t> operationTypeMap = {
    {"exists", OperationType::kExists},
    {"remove", OperationType::kRemove},
    {"range", OperationType::kRange},
    {"expire", OperationType::kExpire},
    {"get", OperationType::kGet},
    {"set", OperationType::kSet},
    {"sadd", OperationType::kSAdd},
    {"srem", OperationType::kSRem},
    {"smembers", OperationType::kSMembers},
    {"zadd", OperationType::kZAdd},
    {"zrem", OperationType::kZRem},
    {"zscore", OperationType::kZScore},
    {"zrank", OperationType::kZRank},
    {"zcard", OperationType::kZCard},
    {"zincr_by", OperationType::kZIncrBy},
    {"zrange_by_rank", OperationType::kZRangeByRank},
    {"zrange_by_score", OperationType::kZRangeByScore},
    {"zrem_by_rank", OperationType::kZRemByRank},
    {"zrem_by_score", OperationType::kZRemByScore},
    {"lpush", OperationType::kLPush},
    {"lpop", OperationType::kLPop},
    {"rpush", OperationType::kRPush},
    {"rpop", OperationType::kRPop},
    {"lrange", OperationType::kLRange},
    {"lget", OperationType::kLGet},
    {"lsize", OperationType::kLSize},
    {"lpopp_n", OperationType::kLPopN},
    {"rpopp_n", OperationType::kRPopN},
    {"hset", OperationType::kHSet},
    {"hget", OperationType::kHGet},
    {"hdel", OperationType::kHDel},
    {"hkeys", OperationType::kHKeys},
    {"hvalues", OperationType::kHValues},
    {"hentries", OperationType::kHEntries}};
inline std::unordered_set<uint8_t> read_types={
    OperationType::kExists,
    OperationType::kGet,
    OperationType::kSMembers,
    OperationType::kZScore,
    OperationType::kZRank,
    OperationType::kZCard,
    OperationType::kZRangeByRank,
    OperationType::kZRangeByScore,
    OperationType::kLRange,
    OperationType::kLGet,
    OperationType::kLSize,
    OperationType::kHGet,
    OperationType::kHKeys,
    OperationType::kHValues,
    OperationType::kHEntries
};
inline std::unordered_set<uint8_t> write_types={
    OperationType::kRemove,
    OperationType::kRange,
    OperationType::kExpire,
    OperationType::kSet,
    OperationType::kSAdd,
    OperationType::kSRem,
    OperationType::kZAdd,
    OperationType::kZRem,
    OperationType::kZIncrBy,
    OperationType::kZRemByRank,
    OperationType::kZRemByScore,
    OperationType::kLPush,
    OperationType::kLPop,
    OperationType::kRPush,
    OperationType::kRPop,
    OperationType::kLPopN,
    OperationType::kRPopN,
    OperationType::kHSet,
    OperationType::kHDel
};
inline uint8_t stringToOperationType(const std::string &cmd)
{
    std::string lower_cmd = cmd;
    std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);
    auto it = operationTypeMap.find(lower_cmd);
    if (it != operationTypeMap.end())
    {
        return it->second;
    }
    throw std::runtime_error("unknown operation type: " + cmd);
}

#endif