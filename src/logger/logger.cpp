#include "logger/logger.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h> // 如果你想开启异步日志，可以包含这个
#include <filesystem>
#include <iostream>
#include <vector>

void Logger::Init(const std::string &log_dir, LogLevel level, uint64_t rotate_size_mb)
{
    try
    {
        // 1. 创建日志目录
        std::error_code ec;
        if (!std::filesystem::exists(log_dir, ec))
        {
            std::filesystem::create_directories(log_dir, ec);
        }

        std::string log_file_path = (std::filesystem::path(log_dir) / "server.log").string();

        // 2. 创建控制台输出 Sink (带颜色高亮)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // 3. 创建文件滚动 Sink (线程安全)
        // 参数: 路径, 最大文件大小, 保留的历史文件数量
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_path, rotate_size_mb * 1024 * 1024, 10);

        // 4. 将两个 sink 组合成一个 logger
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>("EyakvLogger", sinks.begin(), sinks.end());

        // 5. 设置格式
        // [%Y-%m-%d %H:%M:%S] 等价于你的时间格式
        // [%t] 是线程ID
        // [%^%l%$] 是带颜色的日志级别 (如 INFO, ERROR)
        // %v 是日志实际内容
        logger->set_pattern("[%Y-%m-%d %H:%M:%S] [%t] [%^%l%$] %v");

        // 6. 设置全局级别和刷新策略
        logger->set_level(ConvertLogLevel(level));
        logger->flush_on(spdlog::level::err); // 遇到 ERROR 或更高等级自动立即刷盘

        // 7. 注册为全局默认 Logger
        spdlog::set_default_logger(logger);

        // 可选：设置定期自动刷盘（比如每3秒）
        spdlog::flush_every(std::chrono::seconds(3));

        spdlog::info("Logger initialized successfully in directory: {}", log_dir);
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
}

void Logger::Flush()
{
    spdlog::default_logger()->flush();
}