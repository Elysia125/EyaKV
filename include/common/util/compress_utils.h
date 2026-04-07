#ifndef COMPRESS_UTILS_H
#define COMPRESS_UTILS_H

#include <string>
#include <string_view>
#include <stdexcept>
#include <memory>
#include <lz4.h>

class ICompressor
{
public:
    virtual ~ICompressor() = default;
    /**
     * 压缩数据
     * @param data 待压缩的数据
     * @return 压缩后的数据
     */
    virtual std::string compress(std::string_view data) = 0;
    /**
     * 解压数据
     * @param compressedData 待解压的数据
     * @return 解压后的数据
     */
    virtual std::string decompress(std::string_view compressedData, int maxDecompressedSize = -1) = 0;
};

class LZ4Compressor : public ICompressor
{
public:
    std::string compress(std::string_view data) override
    {
        int maxCompressedSize = LZ4_compressBound(static_cast<int>(data.size()));
        std::string compressedData(maxCompressedSize, '\0');
        int compressedSize = LZ4_compress_default(data.data(), compressedData.data(), static_cast<int>(data.size()), maxCompressedSize);
        if (compressedSize <= 0)
        {
            throw std::runtime_error("LZ4 compression failed");
        }
        compressedData.resize(compressedSize);
        return compressedData;
    }
    std::string decompress(std::string_view compressedData, int maxDecompressedSize = -1) override
    {
        if (maxDecompressedSize == -1)
        {
            maxDecompressedSize = LZ4_decompress_safe(compressedData.data(), nullptr, static_cast<int>(compressedData.size()), 0);
        }
        if (maxDecompressedSize <= 0)
        {
            throw std::runtime_error("LZ4 decompression failed");
        }
        std::string decompressedData(maxDecompressedSize, '\0');
        int decompressedSize = LZ4_decompress_safe(compressedData.data(), decompressedData.data(), static_cast<int>(compressedData.size()), maxDecompressedSize);
        if (decompressedSize <= 0)
        {
            throw std::runtime_error("LZ4 decompression failed");
        }
        decompressedData.resize(decompressedSize);
        return decompressedData;
    }
};

class CompressContext
{
private:
    std::unique_ptr<ICompressor> compressor;

public:
    CompressContext(std::unique_ptr<ICompressor> comp = nullptr)
    {
        if (comp)
        {
            compressor = std::move(comp);
        }
        else
        {
            compressor = std::make_unique<LZ4Compressor>();
        }
    }

    void setCompressor(std::unique_ptr<ICompressor> comp)
    {
        compressor = std::move(comp);
    }

    std::string compress(std::string_view data)
    {
        return compressor->compress(data);
    }

    std::string decompress(std::string_view compressedData, int maxDecompressedSize = -1)
    {
        return compressor->decompress(compressedData, maxDecompressedSize);
    }
};
#endif // COMPRESS_UTILS_H