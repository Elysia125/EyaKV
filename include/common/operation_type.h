#ifndef OPERATION_TYPE_H_
#define OPERATION_TYPE_H_

#include <cstdint>

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

#endif