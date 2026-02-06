#ifndef LOGGER_H
#define LOGGER_H
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <ctime>
#include <thread>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <type_traits>
#include "config/config.h"
#include "common/base/export.h"

// 日志工具类（单例 + 按级别分类存储）
class EYAKV_LOGGER_API Logger
{
public:
    // 禁用拷贝构造和赋值运算符
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // 获取单例实例（线程安全的懒汉式）
    static Logger &GetInstance();

    // 初始化日志：指定日志目录 + 输出级别（低于该级别的日志不输出）
    // 示例：Init("./log", LogLevel::DEBUG) → 输出所有级别，且分级存储到./log下
    void Init(const std::string &log_dir, LogLevel level = LogLevel::INFO, uint64_t rotate_size = 1024 * 1024 * 5);

    // 辅助：转换参数为 const char*
    template <typename T>
    const char* to_c_string(T&& arg) {
        if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
            return arg.c_str();
        } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> || std::is_same_v<std::decay_t<T>, char*>) {
            return arg;
        } else {
            return reinterpret_cast<const char*>(arg);
        }
    }

    // 核心日志写入接口：按级别路由到对应文件
    template <typename... Args>
    void Log(LogLevel level, const char *format, Args &&...args)
    {
        // 级别过滤：低于设定级别则不输出
        if (level < log_level_)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mtx_);
        if (!is_init_)
        {
            std::cerr << "Logger not initialized, outputting to stderr" << std::endl;
            OutputToStderr(level, format, std::forward<Args>(args)...);
            return;
        }

        // 1. 获取日志头部（时间+线程ID+级别）
        std::string header = GetLogHeader(level);

        // 2. 根据级别选择对应的文件句柄
        FILE *target_fp = GetFileHandleByLevel(level);
        if (target_fp == nullptr)
        {
            target_fp = stderr; // 降级到stderr
        }

        // 3. 写入内核缓冲区（不手动刷盘）
        fprintf(target_fp, "%s ", header.c_str());
        if constexpr (sizeof...(Args) == 0)
        {
            fprintf(target_fp, "%s", format);
        }
        else
        {
            fprintf(target_fp, format, to_c_string(std::forward<Args>(args))...);
        }
        fprintf(target_fp, "\n");
        if (target_fp != stderr)
        {
            // 打印到控制台
            fprintf(stdout, "%s ", header.c_str());
            if constexpr (sizeof...(Args) == 0)
            {
                fprintf(stdout, "%s", format);
            }
            else
            {
                fprintf(stdout, format, to_c_string(std::forward<Args>(args))...);
            }
            fprintf(stdout, "\n");
        }
        // 4. FATAL级别强制刷盘并退出
        if (level == LogLevel::FATAL)
        {
            fflush(target_fp);
            CloseAllLogFiles(); // 退出前关闭所有文件
            exit(EXIT_FAILURE);
        }
    }

    // 便捷接口（保持原有调用方式不变）
    template <typename... Args>
    void Debug(const char *format, Args &&...args)
    {
        Log(LogLevel::DEBUG, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Info(const char *format, Args &&...args)
    {
        Log(LogLevel::INFO, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Warn(const char *format, Args &&...args)
    {
        Log(LogLevel::WARN, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Error(const char *format, Args &&...args)
    {
        Log(LogLevel::ERROR, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Fatal(const char *format, Args &&...args)
    {
        Log(LogLevel::FATAL, format, std::forward<Args>(args)...);
    }

    // 析构函数：关闭所有文件句柄，刷盘
    ~Logger();

private:
    // 私有构造函数（单例）
    Logger();

    // 辅助：创建目录（跨平台）
    void CreateDir(const std::string &dir);

    // 辅助：打开指定的日志文件（追加模式，内核缓冲区）
    FILE *OpenLogFile(const std::string &filename);

    // 辅助：根据级别获取对应的文件句柄
    FILE *GetFileHandleByLevel(LogLevel level);

    // 辅助：轮转日志文件
    void RotateLogFile(FILE **old_file, const std::string &old_filename);

    // 辅助：生成日志头部
    std::string GetLogHeader(LogLevel level) const;

    // 辅助：未初始化时输出到stderr
    template <typename... Args>
    void OutputToStderr(LogLevel level, const char *format, Args &&...args)
    {
        std::string header = GetLogHeader(level);
        fprintf(stderr, "%s ", header.c_str());
        if constexpr (sizeof...(Args) == 0)
        {
            fprintf(stderr, "%s", format);
        }
        else
        {
            fprintf(stderr, format, to_c_string(std::forward<Args>(args))...);
        }
        fprintf(stderr, "\n");
    }

    // 辅助：关闭所有日志文件句柄并刷盘
    void CloseAllLogFiles();

    // 成员变量
    std::string log_dir_;           // 日志目录（存储所有级别日志文件）
    LogLevel log_level_;            // 全局日志输出级别（过滤低级别日志）
    bool is_init_;                  // 是否初始化
    mutable std::mutex mtx_;        // 线程安全锁
    uint64_t log_rotate_size_; // 日志轮转大小（字节）
    // 各级别对应的文件句柄（核心：按级别分类存储）
    FILE *debug_fp_;
    FILE *info_fp_;
    FILE *warn_fp_;
    FILE *error_fp_;
    FILE *fatal_fp_;
};

// 全局便捷宏（调用方式完全不变）
#define LOG_DEBUG(...) Logger::GetInstance().Debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::GetInstance().Info(__VA_ARGS__)
#define LOG_WARN(...) Logger::GetInstance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::GetInstance().Error(__VA_ARGS__)
#define LOG_FATAL(...) Logger::GetInstance().Fatal(__VA_ARGS__)

#endif