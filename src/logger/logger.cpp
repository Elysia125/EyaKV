#include "logger/logger.h"
#include <chrono>

Logger &Logger::GetInstance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() 
    : log_level_(LogLevel::INFO), 
      is_init_(false),
      log_rotate_size_(5 * 1024 * 1024) // 默认 5MB
{
    file_ptrs_.fill(nullptr);
    file_sizes_.fill(0);
}

Logger::~Logger()
{
    std::lock_guard<std::mutex> lock(mtx_);
    CloseAllLogFiles();
}

void Logger::Init(const std::string &log_dir, LogLevel level, uint64_t rotate_size_mb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    log_level_ = level;
    log_dir_ = log_dir;
    log_rotate_size_ = rotate_size_mb * 1024 * 1024; // 转换为字节

    // 1. 创建日志目录
    CreateDir(log_dir_);

    // 2. 关闭可能存在的旧文件句柄
    CloseAllLogFiles();

    // 3. 为每个级别打开对应的日志文件，并初始化文件大小
    for (size_t i = 0; i < LEVEL_COUNT; ++i)
    {
        LogLevel curr_level = static_cast<LogLevel>(i);
        const char *filename = GetLevelFilename(curr_level);
        file_ptrs_[i] = OpenLogFile(filename);

        // 初始化文件大小跟踪（如果是追加打开，需要知道已有大小）
        if (file_ptrs_[i] != nullptr)
        {
            fseek(file_ptrs_[i], 0, SEEK_END);
            file_sizes_[i] = ftell(file_ptrs_[i]);
        }
    }

    is_init_ = true;
    std::cout << "Logger initialized in directory: " << log_dir_ << std::endl;
}

void Logger::CreateDir(const std::string &dir)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
    {
        std::filesystem::create_directories(dir, ec);
    }
}

FILE *Logger::OpenLogFile(const std::string &filename)
{
    std::filesystem::path full_path = std::filesystem::path(log_dir_) / filename;
    FILE *fp = fopen(full_path.string().c_str(), "a");
    if (fp == nullptr)
    {
        std::cerr << "Open log file failed: " << full_path.string() << ", falling back to stderr" << std::endl;
        return nullptr;
    }
    return fp;
}

void Logger::CheckAndRotate(LogLevel level)
{
    size_t idx = static_cast<size_t>(level);
    if (file_ptrs_[idx] != nullptr && file_sizes_[idx] >= log_rotate_size_)
    {
        RotateLogFile(level, GetLevelFilename(level));
    }
}

void Logger::RotateLogFile(LogLevel level, const std::string &filename)
{
    size_t idx = static_cast<size_t>(level);
    FILE *&old_file = file_ptrs_[idx];

    // 1. 刷盘并关闭旧文件
    if (old_file != nullptr)
    {
        fflush(old_file);
        fclose(old_file);
        old_file = nullptr;
    }

    // 2. 重命名旧文件（增加时间戳后缀）
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::filesystem::path old_path = std::filesystem::path(log_dir_) / filename;
    std::string new_filename = filename + "." + std::to_string(now_sec);
    std::filesystem::path new_path = std::filesystem::path(log_dir_) / new_filename;

    std::error_code ec;
    if (std::filesystem::exists(old_path, ec))
    {
        std::filesystem::rename(old_path, new_path, ec);
        if (ec) {
            std::cerr << "Failed to rotate log file: " << ec.message() << std::endl;
        }
    }

    // 3. 重新打开新文件并重置大小追踪
    old_file = OpenLogFile(filename);
    file_sizes_[idx] = 0;
}

std::string Logger::GetLogHeader(LogLevel level) const
{
    // 1. 格式化时间戳（精确到秒）
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

    // 2. 获取线程ID（使用 thread_local 缓存，避免每次格式化消耗性能）
    thread_local std::string tid_str = []() {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return std::string(buf);
    }();

    // 3. 获取级别字符串
    const char *level_str = GetLevelString(level);

    // 4. 拼接并返回（固定缓冲区拼接比 stringstream 更快）
    char header_buf[128];
    snprintf(header_buf, sizeof(header_buf), "[%s] [%s] [%s]", time_buf, tid_str.c_str(), level_str);
    
    return std::string(header_buf);
}

const char *Logger::GetLevelString(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::FATAL: return "FATAL";
    default:              return "UNKNOWN";
    }
}

const char *Logger::GetLevelFilename(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::DEBUG: return "debug.log";
    case LogLevel::INFO:  return "info.log";
    case LogLevel::WARN:  return "warn.log";
    case LogLevel::ERROR: return "error.log";
    case LogLevel::FATAL: return "fatal.log";
    default:              return "unknown.log";
    }
}

void Logger::CloseAllLogFiles()
{
    for (size_t i = 0; i < LEVEL_COUNT; ++i)
    {
        if (file_ptrs_[i] != nullptr && file_ptrs_[i] != stderr && file_ptrs_[i] != stdout)
        {
            fflush(file_ptrs_[i]);
            fclose(file_ptrs_[i]);
            file_ptrs_[i] = nullptr;
        }
        file_sizes_[i] = 0;
    }
}