#ifndef FILE_UTILS_H_
#define FILE_UTILS_H_

#include <filesystem>

namespace fs = std::filesystem;

// 封装跨平台的创建目录函数（对外接口统一）
inline bool create_directory(const std::string &dir_name)
{
    return fs::create_directory(dir_name);
}

inline bool rename_file(const std::string &old_path, const std::string &new_path)
{
    try
    {
        fs::rename(old_path, new_path);
        return true;
    }
    catch (const fs::filesystem_error &e)
    {
        std::cerr << "Failed to rename file from " << old_path << " to " << new_path << std::endl;
        return false;
    }
}

inline bool remove_file(const std::string &path)
{
    return fs::remove(path);
}
#endif