#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <string>
#include <iostream>
#include <cstdlib>
#include <minwindef.h>
#include <libloaderapi.h>

// 路径工具类：获取可执行文件目录，拼接目标文件绝对路径
class PathUtils {
public:
    // 获取可执行文件的绝对路径（Linux/Windows通用）
    static std::string GetExeAbsolutePath() {
#ifdef _WIN32
        // Windows平台：使用GetModuleFileName获取可执行文件路径
        char exe_path[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        return std::string(exe_path);
#else
        // Linux平台：读取/proc/self/exe符号链接（指向当前进程的可执行文件）
        char exe_path[1024] = {0};
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len == -1) {
            std::cerr << "Error: 获取可执行文件路径失败！" << std::endl;
            return "";
        }
        return std::string(exe_path, len);
#endif
    }

    // 获取可执行文件所在的目录（去掉可执行文件名）
    static std::string GetExeDir() {
        std::string exe_path = GetExeAbsolutePath();
        if (exe_path.empty()) {
            return "./"; // 失败时降级为当前目录
        }

        // 找到最后一个目录分隔符（Linux:/  Windows:\）
#ifdef _WIN32
        size_t sep_pos = exe_path.find_last_of('\\');
#else
        size_t sep_pos = exe_path.find_last_of('/');
#endif

        if (sep_pos == std::string::npos) {
            return "./"; // 无分隔符，返回当前目录
        }

        return exe_path.substr(0, sep_pos + 1); // 保留最后一个分隔符（比如/opt/kvdb/bin/）
    }

    // 核心函数：拼接目标文件的绝对路径（基于可执行文件目录）
    // 参数：relative_path - 相对于可执行文件目录的路径（比如"../log/kv_db.log"）
    static std::string GetTargetFilePath(const std::string& relative_path) {
        std::string exe_dir = GetExeDir();
        // 拼接路径（自动处理重复的分隔符，比如/opt/kvdb/bin/ + ../log → /opt/kvdb/log）
        return CombinePath(exe_dir, relative_path);
    }

private:
    // 辅助函数：拼接两个路径，处理分隔符问题
    static std::string CombinePath(const std::string& dir, const std::string& file) {
        if (dir.empty()) return file;
        if (file.empty()) return dir;

#ifdef _WIN32
        char sep = '\\';
#else
        char sep = '/';
#endif

        // 检查目录末尾是否有分隔符，没有则添加
        std::string full_path = dir;
        if (full_path.back() != sep) {
            full_path += sep;
        }

        // 拼接文件路径
        full_path += file;

        // 简化路径（处理../ ./ 等），比如 /opt/kvdb/bin/../log → /opt/kvdb/log
        char resolved_path[1024] = {0};
#ifdef _WIN32
        _fullpath(resolved_path, full_path.c_str(), sizeof(resolved_path));
#else
        realpath(full_path.c_str(), resolved_path);
#endif

        return std::string(resolved_path);
    }
};

#endif // PATH_UTILS_H