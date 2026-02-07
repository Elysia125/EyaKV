#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <string>
#include <iostream>
#include <cstdlib>
#include <optional>
#include <mutex>
#include <stdlib.h>
#include <filesystem>
#include "common/base/export.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif __linux__
#ifdef __cplusplus
extern "C"
{
#endif
#include <unistd.h> // C 头文件，用 extern "C" 包裹避免命名空间问题
#ifdef __cplusplus
}
#endif
#include <limits.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#endif
// 路径工具类：获取可执行文件目录，拼接目标文件绝对路径
class EYAKV_COMMON_API PathUtils
{
private:
        PathUtils() = delete;
        static std::optional<std::string> exe_dir_;
        static std::mutex cache_mutex_;

public:
        // 获取可执行文件的绝对路径（跨平台通用）
#ifdef _WIN32
        static std::string get_exe_absolute_path()
        {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                char buffer[MAX_PATH];
                GetModuleFileNameA(NULL, buffer, MAX_PATH);
                std::string exe_path = std::string(buffer);
                if (!exe_path.empty())
                {
                        std::cout << "PathUtils: Executable path is " << exe_path << std::endl;
                        return exe_path;
                }
                else
                {
                        std::cerr << "PathUtils: Failed to get executable path on Windows." << std::endl;
                        return "";
                }
        }
#elif __linux__
        static std::string get_exe_absolute_path()
        {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                char buffer[PATH_MAX];
                ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
                if (len != -1 && len < sizeof(buffer))
                {
                        buffer[len] = '\0';
                        std::string exe_path = std::string(buffer);
                        if (!exe_path.empty())
                        {
                                std::cout << "PathUtils: Executable path is " << exe_path << std::endl;
                                return exe_path;
                        }
                }
                std::cerr << "PathUtils: Failed to get executable path on LINUX." << std::endl;
                return "";
        }
#elif __APPLE__
        static std::string get_exe_absolute_path()
        {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                // Use PATH_MAX for consistency and larger buffer
                char buffer[PATH_MAX];
                uint32_t size = sizeof(buffer);
                if (_NSGetExecutablePath(buffer, &size) == 0)
                {
                        std::string exe_path = std::string(buffer);
                        if (!exe_path.empty())
                        {
                                std::cout << "PathUtils: Executable path is " << exe_path << std::endl;
                                return exe_path;
                        }
                }
        }
        std::cerr << "PathUtils: Failed to get executable path on MACOS." << std::endl;
        return "";
}
#else
#error "Platform not supported"
#endif

        // 获取可执行文件所在的目录（去掉可执行文件名）
        static std::string get_exe_dir()
        {
                if (exe_dir_.has_value())
                {
                        return exe_dir_.value();
                }
                std::string exe_path = get_exe_absolute_path();
                if (exe_path.empty())
                {
                        return "./"; // 失败时降级为当前目录
                }

                // 找到最后一个目录分隔符（Linux:/  Windows:\）
#ifdef _WIN32
                size_t sep_pos = exe_path.find_last_of('\\');
#else
                size_t sep_pos = exe_path.find_last_of('/');
#endif

                if (sep_pos == std::string::npos)
                {
                        return "./"; // 无分隔符，返回当前目录
                }

                std::string exe_dir = exe_path.substr(0, sep_pos);
                // 找到倒数第二个目录分隔符（Linux:/  Windows:\）
#ifdef _WIN32
                sep_pos = exe_dir.find_last_of('\\');
#else
                sep_pos = exe_dir.find_last_of('/');
#endif
                if (sep_pos != std::string::npos)
                {
                        exe_dir = exe_dir.substr(0, sep_pos + 1); // 保留最后一个分隔符
                }
                else
                {
#ifdef _WIN32
                        exe_dir += "\\";
#else
                        exe_dir += "/";
#endif
                }
                exe_dir_ = exe_dir;
                return exe_dir_.value();
        }

        // 核心函数：拼接目标文件的绝对路径（基于可执行文件目录）
        // 参数：relative_path - 相对于可执行文件目录的路径（比如"../log/kv_db.log"）
        static std::string get_target_file_path(const std::string &relative_path)
        {
                std::string exe_dir = get_exe_dir();
                // 拼接路径（自动处理重复的分隔符，比如/opt/kvdb/bin/ + ../log → /opt/kvdb/log）
                return combine_path(exe_dir, relative_path);
        }

        // 拼接两个路径，处理分隔符问题
        static std::string combine_path(const std::string &dir, const std::string &file)
        {
                if (dir.empty())
                        return file;
                if (file.empty())
                        return dir;

#ifdef _WIN32
                char sep = '\\';
#else
                char sep = '/';
#endif

                // 检查目录末尾是否有分隔符，没有则添加
                std::string full_path = dir;
                if (full_path.back() != sep)
                {
                        full_path += sep;
                }

                // 拼接文件路径
                full_path += file;

                // 简化路径（处理../ ./ 等），比如 /opt/kvdb/bin/../log → /opt/kvdb/log
                // 注意：realpath() 要求路径必须实际存在，否则返回 nullptr
                // 这里我们先尝试解析，如果失败则使用简化的路径拼接（不验证文件是否存在）
                char resolved_path[PATH_MAX] = {0};
                char *result = nullptr;

#ifdef _WIN32
                result = _fullpath(resolved_path, full_path.c_str(), sizeof(resolved_path));
#else
                // 对于Linux/macOS，使用realpath需要文件存在
                // 我们先检查文件/目录是否存在，如果不存在则手动简化路径
                if (std::filesystem::exists(full_path))
                {
                        result = realpath(full_path.c_str(), resolved_path);
                }
                else
                {
                        // 文件不存在，使用std::filesystem::weakly_canonical来简化路径
                        // 这不会验证文件是否存在，但会解析..和.
                        try
                        {
                                std::filesystem::path path(full_path);
                                std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
                                return canonical.string();
                        }
                        catch (const std::filesystem::filesystem_error& e)
                        {
                                // 如果weakly_canonical也失败，返回原始路径
                                return full_path;
                        }
                }
#endif

                if (result == nullptr)
                {
                        // realpath失败，返回简化后的路径但不警告（文件可能不存在是正常的）
                        return full_path;
                }

                return std::string(resolved_path);
        }
};
#endif // PATH_UTILS_H