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
#include <config/config.h>

// 日志工具类（单例 + 按级别分类存储）
class Logger
{
public:
    // 禁用拷贝构造和赋值运算符
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // 获取单例实例（线程安全的懒汉式）
    static Logger &GetInstance()
    {
        static Logger instance;
        return instance;
    }

    // 初始化日志：指定日志目录 + 输出级别（低于该级别的日志不输出）
    // 示例：Init("./log", LogLevel::DEBUG) → 输出所有级别，且分级存储到./log下
    void Init(const std::string &log_dir, LogLevel level = LogLevel::INFO)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        log_level_ = level;
        log_dir_ = log_dir;

        // 1. 创建日志目录（不存在则创建）
        CreateDir(log_dir_);

        // 2. 关闭原有文件句柄（防止重复初始化）
        CloseAllLogFiles();

        // 3. 为每个级别打开对应的日志文件（内核缓冲区模式）
        debug_fp_ = OpenLogFile("debug.log");
        info_fp_ = OpenLogFile("info.log");
        warn_fp_ = OpenLogFile("warn.log");
        error_fp_ = OpenLogFile("error.log");
        fatal_fp_ = OpenLogFile("fatal.log");

        is_init_ = true;
        std::cout << "日志初始化成功，日志目录：" << log_dir_ << std::endl;
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
            std::cerr << "日志未初始化！所有日志输出到stderr" << std::endl;
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
        fprintf(target_fp, format, std::forward<Args>(args)...);
        fprintf(target_fp, "\n");

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
    ~Logger()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        CloseAllLogFiles();
    }

private:
    // 私有构造函数（单例）
    Logger() : log_level_(LogLevel::INFO), is_init_(false),
               debug_fp_(nullptr), info_fp_(nullptr), warn_fp_(nullptr),
               error_fp_(nullptr), fatal_fp_(nullptr) {}

    // 辅助：创建目录（跨平台）
    void CreateDir(const std::string &dir)
    {
        if (!std::filesystem::exists(dir))
        {
            std::filesystem::create_directories(dir);
        }
    }

    // 辅助：打开指定的日志文件（追加模式，内核缓冲区）
    FILE *OpenLogFile(const std::string &filename)
    {
        std::string full_path = log_dir_ + "/" + filename;
        FILE *fp = fopen(full_path.c_str(), "a");
        if (fp == nullptr)
        {
            std::cerr << "打开日志文件失败：" << full_path << "，降级到stderr" << std::endl;
            return stderr;
        }
        return fp;
    }

    // 辅助：根据级别获取对应的文件句柄
    FILE *GetFileHandleByLevel(LogLevel level) const
    {
        switch (level)
        {
        case LogLevel::DEBUG:
            return debug_fp_;
        case LogLevel::INFO:
            return info_fp_;
        case LogLevel::WARN:
            return warn_fp_;
        case LogLevel::ERROR:
            return error_fp_;
        case LogLevel::FATAL:
            return fatal_fp_;
        default:
            return stderr;
        }
    }

    // 辅助：生成日志头部
    std::string GetLogHeader(LogLevel level) const
    {
        // 时间戳（精确到秒）
        time_t now = time(nullptr);
        tm local_tm;
#ifdef _WIN32
        localtime_s(&local_tm, &now);
#else
        localtime_r(&now, &local_tm);
#endif
        char time_buf[64] = {0};
        snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);

        // 线程ID
        std::ostringstream tid_ss;
        tid_ss << std::this_thread::get_id();

        // 级别字符串
        const char *level_str = nullptr;
        switch (level)
        {
        case LogLevel::DEBUG:
            level_str = "DEBUG";
            break;
        case LogLevel::INFO:
            level_str = "INFO";
            break;
        case LogLevel::WARN:
            level_str = "WARN";
            break;
        case LogLevel::ERROR:
            level_str = "ERROR";
            break;
        case LogLevel::FATAL:
            level_str = "FATAL";
            break;
        default:
            level_str = "UNKNOWN";
        }

        // 拼接头部
        std::ostringstream header_ss;
        header_ss << "[" << time_buf << "] [" << tid_ss.str() << "] [" << level_str << "]";
        return header_ss.str();
    }

    // 辅助：未初始化时输出到stderr
    template <typename... Args>
    void OutputToStderr(LogLevel level, const char *format, Args &&...args)
    {
        std::string header = GetLogHeader(level);
        fprintf(stderr, "%s ", header.c_str());
        fprintf(stderr, format, std::forward<Args>(args)...);
        fprintf(stderr, "\n");
    }

    // 辅助：关闭所有日志文件句柄并刷盘
    void CloseAllLogFiles()
    {
        // 定义要关闭的文件句柄列表
        FILE *fps[] = {debug_fp_, info_fp_, warn_fp_, error_fp_, fatal_fp_};
        for (FILE *fp : fps)
        {
            if (fp != nullptr && fp != stderr && fp != stdout)
            {
                fflush(fp); // 刷盘
                fclose(fp);
            }
        }
        // 重置句柄
        debug_fp_ = info_fp_ = warn_fp_ = error_fp_ = fatal_fp_ = nullptr;
    }

    // 成员变量
    std::string log_dir_;    // 日志目录（存储所有级别日志文件）
    LogLevel log_level_;     // 全局日志输出级别（过滤低级别日志）
    bool is_init_;           // 是否初始化
    mutable std::mutex mtx_; // 线程安全锁

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