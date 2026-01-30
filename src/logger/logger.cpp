#include "logger/logger.h"

// 获取单例实例（线程安全的懒汉式）
Logger &Logger::GetInstance()
{
    static Logger instance;
    return instance;
}

// 初始化日志
void Logger::Init(const std::string &log_dir, LogLevel level, unsigned long rotate_size)
{
    std::lock_guard<std::mutex> lock(mtx_);
    log_level_ = level;
    log_dir_ = log_dir;
    log_rotate_size_ = rotate_size * 1024; // 转换为字节
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

// 析构函数
Logger::~Logger()
{
    std::lock_guard<std::mutex> lock(mtx_);
    CloseAllLogFiles();
}

// 私有构造函数
Logger::Logger() : log_level_(LogLevel::INFO), is_init_(false),
                   debug_fp_(nullptr), info_fp_(nullptr), warn_fp_(nullptr),
                   error_fp_(nullptr), fatal_fp_(nullptr) {}

// 辅助：创建目录
void Logger::CreateDir(const std::string &dir)
{
    if (!std::filesystem::exists(dir))
    {
        std::filesystem::create_directories(dir);
    }
}

// 辅助：打开指定的日志文件
FILE *Logger::OpenLogFile(const std::string &filename)
{
    std::string full_path = PathUtils::combine_path(log_dir_, filename);
    FILE *fp = fopen(full_path.c_str(), "a");
    if (fp == nullptr)
    {
        std::cerr << "打开日志文件失败：" << full_path << "，降级到stderr" << std::endl;
        return stderr;
    }
    return fp;
}

// 辅助：根据级别获取对应的文件句柄
FILE *Logger::GetFileHandleByLevel(LogLevel level)
{
    FILE **file = nullptr;
    std::string old_filename;
    switch (level)
    {
    case LogLevel::DEBUG:
        file = &debug_fp_;
        old_filename = "debug.log";
        break;
    case LogLevel::INFO:
        file = &info_fp_;
        old_filename = "info.log";
        break;
    case LogLevel::WARN:
        file = &warn_fp_;
        old_filename = "warn.log";
        break;
    case LogLevel::ERROR:
        file = &error_fp_;
        old_filename = "error.log";
        break;
    case LogLevel::FATAL:
        file = &fatal_fp_;
        old_filename = "fatal.log";
        break;
    default:
        return stderr;
    }
    // 检查文件大小，是否需要轮转
    fseek(*file, 0, SEEK_END);
    long file_size = ftell(*file);
    if (file_size >= log_rotate_size_)
    {
        RotateLogFile(file, old_filename);
    }
    return *file;
}

// 辅助：轮转日志文件
void Logger::RotateLogFile(FILE **old_file, const std::string &old_filename)
{
    fflush(*old_file);
    fclose(*old_file);

    std::string full_path = PathUtils::combine_path(log_dir_, old_filename);
    std::string new_filename = old_filename + ".1";
    std::string full_new_path = PathUtils::combine_path(log_dir_, new_filename);
    std::filesystem::rename(full_path, full_new_path);
    *old_file = OpenLogFile(old_filename);
}

// 辅助：生成日志头部
std::string Logger::GetLogHeader(LogLevel level) const
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

// 辅助：关闭所有日志文件句柄并刷盘
void Logger::CloseAllLogFiles()
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
