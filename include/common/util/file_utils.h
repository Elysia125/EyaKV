#ifndef FILE_UTILS_H_
#define FILE_UTILS_H_

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <memory>

namespace fs = std::filesystem;
// 自定义文件句柄删除器
struct FileCloser
{
    void operator()(FILE *fp) const
    {
        if (fp != nullptr)
        {
            fclose(fp);
        }
    }
};
using UniqueFilePtr = std::unique_ptr<FILE, FileCloser>;

// 辅助函数：获取文件大小（文件不存在返回0）
inline uint64_t get_file_size(const std::string &file_path)
{
    if (!fs::exists(file_path))
    {
        return 0;
    }
    return fs::file_size(file_path);
}
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
/**
 * @brief 压缩与解压缩策略接口
 */
// 1. 抽象策略接口
class ICompressionStrategy
{
public:
    virtual ~ICompressionStrategy() = default;

    /**
     * 压缩目录
     * @param sourceDir 需要压缩的源目录
     * @param destTarGzFile 生成的 .tar.gz 文件路径
     * @return 成功返回 true
     */
    virtual bool compress(const std::string &sourceDir, const std::string &destTarGzFile) = 0;

    /**
     * 解压缩文件
     * @param sourceTarGzFile .tar.gz 源文件路径
     * @param destDir 解压的目标目录
     * @return 成功返回 true
     */
    virtual bool decompress(const std::string &sourceTarGzFile, const std::string &destDir) = 0;
};

// 2. 具体策略：系统命令策略
//    利用 Windows/Linux/MacOS 自带的 tar 命令
class SystemTarGzStrategy : public ICompressionStrategy
{
private:
    // 辅助函数：处理路径中的空格，给路径加引号
    std::string quote(const std::string &path)
    {
        return "\"" + path + "\"";
    }

    /**
     * 检查 tar 命令是否可用
     */
    bool isTarAvailable()
    {
        // 执行 tar --version，如果返回 0 表示命令存在
        // > NUL (Windows) 或 > /dev/null (Linux/Mac) 用于屏蔽输出
#ifdef _WIN32
        int ret = std::system("tar --version > NUL 2>&1");
#else
        int ret = std::system("tar --version > /dev/null 2>&1");
#endif
        return ret == 0;
    }

public:
    bool compress(const std::string &sourceDir, const std::string &destTarGzFile) override
    {
        if (!isTarAvailable())
        {
            std::cerr << "Error: tar utility is not available on this system." << std::endl;
            std::cerr << "Please install tar utility and try again." << std::endl;
            return false;
        }
        fs::path srcPath(sourceDir);
        fs::path destPath(destTarGzFile);

        // 1. 检查源目录是否存在
        if (!fs::exists(srcPath) || !fs::is_directory(srcPath))
        {
            std::cerr << "Error: Source directory does not exist or is not a directory: " << sourceDir << std::endl;
            return false;
        }

        // 2. 获取绝对路径，防止 cd 切换目录后找不到文件
        srcPath = fs::absolute(srcPath);
        destPath = fs::absolute(destPath);

        // 3. 构建命令
        // 逻辑：cd 到源目录内部，然后压缩当前目录下的所有内容 (.) 到目标文件
        // 这样可以避免压缩包内包含根目录层级
        std::string cmd;

#ifdef _WIN32
        // Windows 命令: cd /d 确保可以跨盘符切换
        cmd = "cd /d " + quote(srcPath.string()) + " && tar -czf " + quote(destPath.string()) + " .";
#else
        // Linux/MacOS 命令
        cmd = "cd " + quote(srcPath.string()) + " && tar -czf " + quote(destPath.string()) + " .";
#endif

        std::cout << "Executing command: " << cmd << std::endl;

        // 4. 执行命令
        int ret = std::system(cmd.c_str());
        return ret == 0;
    }

    bool decompress(const std::string &sourceTarGzFile, const std::string &destDir) override
    {
        if (!isTarAvailable())
        {
            std::cerr << "Error: tar utility is not available on this system." << std::endl;
            std::cerr << "Please install tar utility and try again." << std::endl;
            return false;
        }
        fs::path srcPath(sourceTarGzFile);
        fs::path destPath(destDir);

        // 1. 检查源文件
        if (!fs::exists(srcPath))
        {
            std::cerr << "Error: Compressed file does not exist: " << sourceTarGzFile << std::endl;
            return false;
        }

        // 2. 确保目标目录存在，不存在则创建
        if (!fs::exists(destPath))
        {
            try
            {
                fs::create_directories(destPath);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Failed to create destination directory: " << e.what() << std::endl;
                return false;
            }
        }

        srcPath = fs::absolute(srcPath);
        destPath = fs::absolute(destPath);

        // 3. 构建命令
        // -x: 解压, -z: gzip, -f: 文件, -C: 切换目录
        std::string cmd;

        // 注意：Windows 的 tar 也支持 -C 参数
        cmd = "tar -xzf " + quote(srcPath.string()) + " -C " + quote(destPath.string());

        std::cout << "Executing command: " << cmd << std::endl;

        // 4. 执行
        int ret = std::system(cmd.c_str());
        return ret == 0;
    }
};

// 3. 上下文类
class Archiver
{
private:
    std::unique_ptr<ICompressionStrategy> strategy;

public:
    // 构造函数注入策略，默认为系统命令策略
    Archiver(std::unique_ptr<ICompressionStrategy> strategy = nullptr)
    {
        if (strategy)
        {
            this->strategy = std::move(strategy);
        }
        else
        {
            // 默认策略
            this->strategy = std::make_unique<SystemTarGzStrategy>();
        }
    }

    void setStrategy(std::unique_ptr<ICompressionStrategy> newStrategy)
    {
        this->strategy = std::move(newStrategy);
    }

    bool compressDir(const std::string &src, const std::string &dest)
    {
        if (!strategy)
            return false;
        std::cout << "[Archiver] Starting compression..." << std::endl;
        return strategy->compress(src, dest);
    }

    bool extractTo(const std::string &src, const std::string &dest)
    {
        if (!strategy)
            return false;
        std::cout << "[Archiver] Starting extraction..." << std::endl;
        return strategy->decompress(src, dest);
    }
};

#endif