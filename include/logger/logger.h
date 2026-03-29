#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <spdlog/spdlog.h>
#include "config/config.h"
#include "common/base/export.h"

class EYAKV_LOGGER_API Logger
{
private:
    static spdlog::level::level_enum ConvertLogLevel(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::DEBUG:
            return spdlog::level::debug;
        case LogLevel::INFO:
            return spdlog::level::info;
        case LogLevel::WARN:
            return spdlog::level::warn;
        case LogLevel::ERROR:
            return spdlog::level::err;
        case LogLevel::FATAL:
            return spdlog::level::critical;
        default:
            return spdlog::level::info; // 默认级别
        }
    }

public:
    // 禁用实例化，作为配置类使用
    Logger() = delete;

    /**
     * @brief 初始化日志系统 (使用 spdlog 底层)
     * @param log_dir 日志文件存储目录
     * @param level 最低输出级别
     * @param rotate_size_mb 单个日志文件最大轮转大小（默认5MB）
     */
    static void Init(const std::string &log_dir, LogLevel level = LogLevel::INFO, uint64_t rotate_size_mb = 5);

    /**
     * @brief 强制将缓存中的日志刷入磁盘
     */
    static void Flush();
};

// 全局便捷宏：直接映射为 spdlog 的 API
// 注意：迁移到 spdlog 后，格式化占位符需要从 %d, %s 改为 {}，并且不需要.c_str()
#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_FATAL(...)                 \
    do                                 \
    {                                  \
        spdlog::critical(__VA_ARGS__); \
        spdlog::shutdown();            \
        std::exit(EXIT_FAILURE);       \
    } while (0)

#endif // LOGGER_H