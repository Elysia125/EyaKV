#ifndef CHECKSUM_UTILS_H
#define CHECKSUM_UTILS_H

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <memory>
#include <iomanip> // 用于 hex 输出


// 1. 策略接口 (Strategy Interface)
class IChecksumStrategy {
public:
    virtual ~IChecksumStrategy() = default;

    // 计算文件的 CRC32
    virtual uint32_t computeFile(const std::string& filePath) = 0;
    
    // 计算字符串/内存数据的 CRC32
    virtual uint32_t computeString(std::string_view data) = 0;
    
    virtual std::string getName() const = 0;
};


// 2. 具体策略：CRC32 (IEEE 802.3)
class CRC32Strategy : public IChecksumStrategy {
private:
    std::vector<uint32_t> table;

    // 初始化 CRC32 表
    void generateTable() {
        uint32_t polynomial = 0xEDB88320;
        table.resize(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1) {
                    c = polynomial ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            table[i] = c;
        }
    }

    /**
     * 核心计算逻辑 (内部复用)
     * @param crc 当前的 CRC 值
     * @param data 数据指针
     * @param length 数据长度
     * @return 更新后的 CRC 值
     */
    uint32_t update(uint32_t crc, const char* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            uint8_t byte = static_cast<uint8_t>(data[i]);
            uint32_t lookupIndex = (crc ^ byte) & 0xFF;
            crc = (crc >> 8) ^ table[lookupIndex];
        }
        return crc;
    }

public:
    CRC32Strategy() {
        generateTable();
    }

    std::string getName() const override {
        return "CRC32";
    }

    // 实现：计算内存字符串
    uint32_t computeString(std::string_view data) override {
        uint32_t crc = 0xFFFFFFFF; // 初始值
        crc = update(crc, data.data(), data.size());
        return ~crc; // 结果取反
    }

    // 实现：计算文件
    uint32_t computeFile(const std::string& filePath) override {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Cannot open file:" << filePath << std::endl;
            return 0;
        }

        uint32_t crc = 0xFFFFFFFF;
        const size_t bufferSize = 8192;
        std::vector<char> buffer(bufferSize);

        while (file.read(buffer.data(), bufferSize) || file.gcount() > 0) {
            // 复用 update 函数
            crc = update(crc, buffer.data(), static_cast<size_t>(file.gcount()));
            if (file.eof()) break;
        }

        return ~crc;
    }
};


// 3. 上下文类 (Context)

class ChecksumCalculator {
private:
    std::unique_ptr<IChecksumStrategy> strategy;

public:
    ChecksumCalculator(std::unique_ptr<IChecksumStrategy> strategy = nullptr) {
        if (strategy) this->strategy = std::move(strategy);
        else this->strategy = std::make_unique<CRC32Strategy>();
    }

    uint32_t fromFile(const std::string& path) {
        return strategy ? strategy->computeFile(path) : 0;
    }

    uint32_t fromString(std::string_view data) {
        return strategy ? strategy->computeString(data) : 0;
    }
};

#endif