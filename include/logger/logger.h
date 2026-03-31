#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <spdlog/spdlog.h>
#include "config/config.h"
#include "common/base/export.h"

// 日志全局配置（外部可修改，控制控制台输出核心）
struct LoggerConfig
{
    LogLevel level = LogLevel::INFO; // 日志级别
    uint64_t rotate_size_mb = 5;     // 单个日志文件最大大小（MB）
    bool enable_console = true;      // 控制台输出开关（核心）
    std::string log_dir = "./logs";  // 日志目录
    int max_backup_files = 10;       // 日志文件保留数量
    int flush_interval_sec = 3;      // 自动刷盘间隔（秒）
};

class EYAKV_LOGGER_API Logger
{
private:
    static spdlog::level::level_enum ConvertLogLevel(LogLevel level);
    static LoggerConfig global_config_; // 静态配置（外部可通过SetConfig修改）
    // 增加一个静态指针，保存初始化后的 Logger 实例
    static std::shared_ptr<spdlog::logger> async_logger_;
    static spdlog::logger *hot_logger_ptr_;

public:
    // 禁用实例化，作为配置类使用
    Logger() = delete;

    /**
     * @brief 设置全局日志配置
     * @param config 日志配置
     */
    static void SetConfig(const LoggerConfig &config);

    /**
     * @brief 初始化日志系统 (使用 spdlog 底层)
     * @param log_dir 日志文件存储目录
     * @param level 最低输出级别
     * @param rotate_size_mb 单个日志文件最大轮转大小（默认5MB）
     */
    static void Init(const std::string &log_dir = "", LogLevel level = LogLevel::INFO, uint64_t rotate_size_mb = 5);

    /**
     * @brief 强制将缓存中的日志刷入磁盘
     */
    static void Flush();

    // 提供获取 Logger 实例的接口（跨 DLL 边界调用）
    static std::shared_ptr<spdlog::logger> &GetInstance();

    // 内联的极速获取接口（无任何分支判断）
    static inline spdlog::logger *GetHotLogger() { return hot_logger_ptr_; }
};

// 全局便捷宏：直接映射为 spdlog 的 API
// 注意：迁移到 spdlog 后，格式化占位符需要从 %d, %s 改为 {}，并且不需要.c_str()
// 1. 获取裸指针
// 2. 宏内部提前进行 level 检查（短路计算），如果级别不够，连后面的参数计算(__VA_ARGS__)都直接跳过！
#define LOG_DEBUG(...)                                  \
    do                                                  \
    {                                                   \
        spdlog::logger *_l = Logger::GetHotLogger();    \
        if (_l && _l->should_log(spdlog::level::debug)) \
        {                                               \
            SPDLOG_LOGGER_DEBUG(_l, __VA_ARGS__);       \
        }                                               \
    } while (0)

#define LOG_INFO(...)                                  \
    do                                                 \
    {                                                  \
        spdlog::logger *_l = Logger::GetHotLogger();   \
        if (_l && _l->should_log(spdlog::level::info)) \
        {                                              \
            SPDLOG_LOGGER_INFO(_l, __VA_ARGS__);       \
        }                                              \
    } while (0)

#define LOG_WARN(...)                                  \
    do                                                 \
    {                                                  \
        spdlog::logger *_l = Logger::GetHotLogger();   \
        if (_l && _l->should_log(spdlog::level::warn)) \
        {                                              \
            SPDLOG_LOGGER_WARN(_l, __VA_ARGS__);       \
        }                                              \
    } while (0)

#define LOG_ERROR(...)                                \
    do                                                \
    {                                                 \
        spdlog::logger *_l = Logger::GetHotLogger();  \
        if (_l && _l->should_log(spdlog::level::err)) \
        {                                             \
            SPDLOG_LOGGER_ERROR(_l, __VA_ARGS__);     \
        }                                             \
    } while (0)

#define LOG_FATAL(...)                                     \
    do                                                     \
    {                                                      \
        spdlog::logger *_l = Logger::GetHotLogger();       \
        if (_l && _l->should_log(spdlog::level::critical)) \
        {                                                  \
            SPDLOG_LOGGER_CRITICAL(_l, __VA_ARGS__);       \
            Logger::Flush();                               \
            spdlog::shutdown();                            \
            std::exit(EXIT_FAILURE);                       \
        }                                                  \
    } while (0)
#endif // LOGGER_H