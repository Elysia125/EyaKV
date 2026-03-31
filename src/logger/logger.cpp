#include "logger/logger.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h> // 异步日志（核心性能优化）
#include <filesystem>
#include <iostream>
#include <vector>

// 初始化静态配置
LoggerConfig Logger::global_config_;
std::shared_ptr<spdlog::logger> Logger::async_logger_ = nullptr; // 初始化为空
spdlog::logger *Logger::hot_logger_ptr_ = nullptr;

void Logger::SetConfig(const LoggerConfig &config)
{
    global_config_ = config;
}

spdlog::level::level_enum Logger::ConvertLogLevel(LogLevel level)
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
        return spdlog::level::info;
    }
}

// 核心：暴露实例获取接口，如果未 Init 则返回自带的安全默认 logger，防止崩溃
std::shared_ptr<spdlog::logger> &Logger::GetInstance()
{
    if (!async_logger_)
    {
        // 如果业务层在 Init 之前就调用了宏，给一个默认兜底。
        async_logger_ = spdlog::default_logger();
        hot_logger_ptr_ = async_logger_.get();
    }
    return async_logger_;
}
void Logger::Init(const std::string &log_dir, LogLevel level, uint64_t rotate_size_mb)
{
    try
    {
        LoggerConfig cfg = global_config_;
        if (!log_dir.empty())
            cfg.log_dir = log_dir;
        if (level != LogLevel::INFO)
            cfg.level = level;
        if (rotate_size_mb != 5)
            cfg.rotate_size_mb = rotate_size_mb;

        std::error_code ec;
        if (!std::filesystem::exists(cfg.log_dir, ec))
        {
            std::filesystem::create_directories(cfg.log_dir, ec);
        }
        std::string log_file = (std::filesystem::path(cfg.log_dir) / "server.log").string();

        std::vector<spdlog::sink_ptr> sinks;

        if (cfg.enable_console)
        {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);
        }

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file,
            cfg.rotate_size_mb * 1024 * 1024,
            cfg.max_backup_files);
        sinks.push_back(file_sink);

        // 防止多次调用 Init 导致 init_thread_pool 抛出异常
        if (!spdlog::thread_pool())
        {
            spdlog::init_thread_pool(8192, 1);
        }

        async_logger_ = std::make_shared<spdlog::async_logger>(
            "EyakvLogger",
            sinks.begin(), sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest);

        async_logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");

        async_logger_->set_level(ConvertLogLevel(cfg.level));
        async_logger_->flush_on(spdlog::level::err);

        spdlog::flush_every(std::chrono::seconds(cfg.flush_interval_sec));

        // 依然设置为DLL内部的默认Logger，方便DLL内部其他未用宏的地方
        spdlog::set_default_logger(async_logger_);
        hot_logger_ptr_ = async_logger_.get(); // 给热路径指针赋值
        // 这里的 SPDLOG_LOGGER_INFO 保证初始化的这条日志严格按照配置走
        SPDLOG_LOGGER_INFO(async_logger_, "Logger init success | console: {} | dir: {}",
                           cfg.enable_console ? "ON" : "OFF", cfg.log_dir);
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
    }
}

void Logger::Flush()
{
    if (async_logger_)
    {
        async_logger_->flush();
    }
}

void Logger::Shutdown()
{
    // 1. 强制刷盘，保证遗留日志不丢失
    if (async_logger_)
    {
        async_logger_->flush();
    }

    // 2. 将热路径指针置空（关键！）
    // 置空后，任何在 Shutdown 之后调用的 LOG_INFO 都会在宏内部的 if (_l) 处被直接短路丢弃
    hot_logger_ptr_ = nullptr;

    // 3. 关闭 spdlog，释放其内部的异步线程池
    spdlog::shutdown();
    async_logger_.reset();
}