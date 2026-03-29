#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <mutex>
#include <array>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <type_traits>
#include "config/config.h"
#include "common/base/export.h"

/**
 * @brief 日志工具类（单例模式 + 按级别分类存储）
 * @details 线程安全，支持日志文件按大小自动轮转。
 */
class EYAKV_LOGGER_API Logger
{
public:
    // 禁用拷贝构造和赋值运算符
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /**
     * @brief 获取全局单例实例（线程安全的懒汉式）
     * @return Logger& 单例引用
     */
    static Logger &GetInstance();

    /**
     * @brief 初始化日志系统
     * @param log_dir 日志文件存储目录
     * @param level 最低输出级别（低于该级别的日志将被丢弃）
     * @param rotate_size 单个日志文件最大轮转大小（默认5MB），单位：MB
     */
    void Init(const std::string &log_dir, LogLevel level = LogLevel::INFO, uint64_t rotate_size = 5);

    /**
     * @brief 核心日志写入接口
     * @tparam Args 可变参数类型
     * @param level 当前日志级别
     * @param format C风格的格式化字符串
     * @param args 可变参数列表
     */
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

        // 1. 检查是否需要轮转日志文件
        CheckAndRotate(level);

        // 2. 获取日志头部（时间+线程ID+级别）
        std::string header = GetLogHeader(level);

        // 3. 根据级别选择对应的文件句柄
        size_t level_idx = static_cast<size_t>(level);
        FILE *target_fp = file_ptrs_[level_idx];
        if (target_fp == nullptr)
        {
            target_fp = stderr; // 降级到stderr
        }

        // 4. 将用户消息格式化到本地 buffer（避免两次格式化带来的性能损耗）
        char user_msg[4096];
        if constexpr (sizeof...(Args) == 0)
        {
            snprintf(user_msg, sizeof(user_msg), "%s", format);
        }
        else
        {
            snprintf(user_msg, sizeof(user_msg), format, FormatArg(std::forward<Args>(args))...);
        }

        // 5. 写入文件（并累加写入字节数）
        int written = fprintf(target_fp, "%s %s\n", header.c_str(), user_msg);
        if (written > 0 && target_fp != stderr)
        {
            file_sizes_[level_idx] += written;
        }

        // 6. 输出到控制台
        if (target_fp != stderr)
        {
            fprintf(stdout, "%s %s\n", header.c_str(), user_msg);
        }

        // 7. FATAL级别强制刷盘并退出
        if (level == LogLevel::FATAL)
        {
            fflush(target_fp);
            CloseAllLogFiles();
            exit(EXIT_FAILURE);
        }
    }

    // --- 便捷接口 ---

    template <typename... Args>
    void Debug(const char *format, Args &&...args) { Log(LogLevel::DEBUG, format, std::forward<Args>(args)...); }

    template <typename... Args>
    void Info(const char *format, Args &&...args) { Log(LogLevel::INFO, format, std::forward<Args>(args)...); }

    template <typename... Args>
    void Warn(const char *format, Args &&...args) { Log(LogLevel::WARN, format, std::forward<Args>(args)...); }

    template <typename... Args>
    void Error(const char *format, Args &&...args) { Log(LogLevel::ERROR, format, std::forward<Args>(args)...); }

    template <typename... Args>
    void Fatal(const char *format, Args &&...args) { Log(LogLevel::FATAL, format, std::forward<Args>(args)...); }

    ~Logger();

private:
    Logger();

    /**
     * @brief 参数转换辅助：安全地处理 std::string 等类型，防止 C 风格格式化崩溃
     */
    template <typename T>
    static decltype(auto) FormatArg(T &&arg)
    {
        using DecayedT = std::decay_t<T>;
        if constexpr (std::is_same_v<DecayedT, std::string>)
        {
            return arg.c_str(); // 将 std::string 转为 const char*
        }
        else
        {
            return std::forward<T>(arg); // 其他基本类型（如 int, float, const char*）直接转发
        }
    }

    /**
     * @brief 未初始化时输出到 stderr 的备用机制
     */
    template <typename... Args>
    void OutputToStderr(LogLevel level, const char *format, Args &&...args)
    {
        std::string header = GetLogHeader(level);
        fprintf(stderr, "%s ", header.c_str());
        if constexpr (sizeof...(Args) == 0)
        {
            fprintf(stderr, "%s\n", format);
        }
        else
        {
            fprintf(stderr, format, FormatArg(std::forward<Args>(args))...);
            fprintf(stderr, "\n");
        }
    }

    void CreateDir(const std::string &dir);
    FILE *OpenLogFile(const std::string &filename);
    void CheckAndRotate(LogLevel level);
    void RotateLogFile(LogLevel level, const std::string &filename);
    std::string GetLogHeader(LogLevel level) const;
    void CloseAllLogFiles();
    const char *GetLevelString(LogLevel level) const;
    const char *GetLevelFilename(LogLevel level) const;

private:
    static constexpr size_t LEVEL_COUNT = 5; // 日志级别总数 (DEBUG, INFO, WARN, ERROR, FATAL)

    std::string log_dir_;      ///< 日志存储目录
    LogLevel log_level_;       ///< 全局最低输出级别
    bool is_init_;             ///< 是否已初始化
    mutable std::mutex mtx_;   ///< 线程同步锁
    uint64_t log_rotate_size_; ///< 单个日志文件轮转大小（字节）

    // 使用 std::array 统一管理文件句柄和对应大小，提升扩展性和可读性
    std::array<FILE *, LEVEL_COUNT> file_ptrs_{};  ///< 各级别日志文件句柄数组
    std::array<size_t, LEVEL_COUNT> file_sizes_{}; ///< 各级别日志文件当前大小跟踪
};

// 全局便捷宏
#define LOG_DEBUG(...) Logger::GetInstance().Debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::GetInstance().Info(__VA_ARGS__)
#define LOG_WARN(...) Logger::GetInstance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::GetInstance().Error(__VA_ARGS__)
#define LOG_FATAL(...) Logger::GetInstance().Fatal(__VA_ARGS__)

#endif // LOGGER_H