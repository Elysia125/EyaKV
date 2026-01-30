#ifndef FILE_UTILS_H_
#define FILE_UTILS_H_

#include <string>
#include <filesystem>
#include <archive.h>
#include <archive_entry.h>

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
    struct stat file_stat{};
    if (stat(file_path.c_str(), &file_stat) != 0)
    {
        // 文件不存在或获取失败，返回0
        return 0;
    }
    return static_cast<uint64_t>(file_stat.st_size);
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

// 错误处理辅助函数
static void check_archive_error(int r, struct archive *a, const std::string &msg)
{
    if (r != ARCHIVE_OK && r != ARCHIVE_EOF)
    {
        throw std::runtime_error(msg + ": " + archive_error_string(a));
    }
}

/**
 * 压缩目录为 tar.gz
 * @param src_dir 待压缩的源目录（绝对/相对路径,压缩后不保留根目录）
 * @param dest_targz 输出的 tar.gz 文件路径（如 "output.tar.gz"）
 */
inline void compress_dir_to_targz(const std::string &src_dir, const std::string &dest_targz)
{
    // 检查源目录是否存在
    if (!fs::is_directory(src_dir))
    {
        throw std::invalid_argument("Source directory does not exist: " + src_dir);
    }

    struct archive *a = archive_write_new();
    if (!a)
    {
        throw std::runtime_error("Failed to create archive writer");
    }

    try
    {
        // 启用 gzip 压缩 + tar 格式
        check_archive_error(
            archive_write_add_filter_gzip(a),
            a,
            "Failed to add gzip filter");
        check_archive_error(
            archive_write_set_format_pax_restricted(a), // 兼容大部分 tar 工具
            a,
            "Failed to set tar format");
        // 打开输出文件
        check_archive_error(
            archive_write_open_filename(a, dest_targz.c_str()),
            a,
            "Failed to open output file: " + dest_targz);

        // 递归遍历目录
        fs::path src_path = fs::absolute(src_dir);
        for (const auto &entry : fs::recursive_directory_iterator(src_path))
        {
            struct archive_entry *ae = archive_entry_new();
            if (!ae)
            {
                throw std::runtime_error("Failed to create archive entry");
            }

            try
            {
                fs::path entry_path = entry.path();
                // 计算归档内的相对路径
                fs::path rel_path = fs::relative(entry_path, src_path);

                // 设置归档条目属性
                archive_entry_set_pathname(ae, rel_path.string().c_str());
                archive_entry_set_size(ae, entry.is_regular_file() ? fs::file_size(entry_path) : 0);
                archive_entry_set_filetype(ae, entry.is_directory() ? AE_IFDIR : AE_IFREG);
                archive_entry_set_perm(ae, 0644); // 默认权限（可根据需求调整）

                // 写入条目头
                check_archive_error(
                    archive_write_header(a, ae),
                    a,
                    "Failed to write header for: " + rel_path.string());

                // 若为文件，写入文件内容
                if (entry.is_regular_file())
                {
                    std::FILE *f = std::fopen(entry_path.string().c_str(), "rb");
                    if (!f)
                    {
                        throw std::runtime_error("Failed to open file: " + entry_path.string());
                    }

                    char buf[8192];
                    size_t bytes_read;
                    while ((bytes_read = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    {
                        check_archive_error(
                            archive_write_data(a, buf, bytes_read),
                            a,
                            "Failed to write data for: " + rel_path.string());
                    }
                    std::fclose(f);
                }

                archive_entry_free(ae);
            }
            catch (...)
            {
                archive_entry_free(ae);
                throw;
            }
        }

        // 完成写入并清理
        check_archive_error(archive_write_close(a), a, "Failed to close archive");
        archive_write_free(a);
        std::cout << "Compressed successfully: " << dest_targz << std::endl;
    }
    catch (...)
    {
        archive_write_free(a);
        throw;
    }
}

/**
 * 解压缩 tar.gz 文件到指定目录
 * @param src_targz 待解压的 tar.gz 文件路径
 * @param dest_dir 解压目标目录（不存在则自动创建）
 */
inline void decompress_targz(const std::string &src_targz, const std::string &dest_dir)
{
    // 检查源文件是否存在
    if (!fs::is_regular_file(src_targz))
    {
        throw std::invalid_argument("Source tar.gz file does not exist: " + src_targz);
    }

    // 创建目标目录（递归创建）
    fs::create_directories(dest_dir);

    struct archive *a = archive_read_new();
    if (!a)
    {
        throw std::runtime_error("Failed to create archive reader");
    }

    try
    {
        // 启用 gzip 解压 + tar 格式解析
        check_archive_error(
            archive_read_support_filter_gzip(a),
            a,
            "Failed to support gzip filter");
        check_archive_error(
            archive_read_support_format_tar(a),
            a,
            "Failed to support tar format");
        // 打开输入文件
        check_archive_error(
            archive_read_open_filename(a, src_targz.c_str(), 8192),
            a,
            "Failed to open input file: " + src_targz);

        struct archive_entry *ae;
        // 遍历归档中的每个条目
        while (archive_read_next_header(a, &ae) == ARCHIVE_OK)
        {
            // 拼接目标路径
            fs::path dest_path = fs::path(dest_dir) / archive_entry_pathname(ae);

            // 若为目录，创建目录
            if (archive_entry_filetype(ae) == AE_IFDIR)
            {
                fs::create_directories(dest_path);
                continue;
            }

            // 若为文件，写入文件内容
            std::FILE *f = std::fopen(dest_path.string().c_str(), "wb");
            if (!f)
            {
                throw std::runtime_error("Failed to create file: " + dest_path.string());
            }

            try
            {
                const void *buf;
                size_t size;
                int64_t offset;
                // 读取条目数据并写入文件
                while (archive_read_data_block(a, &buf, &size, &offset) == ARCHIVE_OK)
                {
                    std::fwrite(buf, 1, size, f);
                }
                std::fclose(f);
            }
            catch (...)
            {
                std::fclose(f);
                throw;
            }

            // 设置文件权限（可选）
            fs::permissions(dest_path, fs::perms::owner_read | fs::perms::owner_write);
        }

        // 完成读取并清理
        check_archive_error(archive_read_close(a), a, "Failed to close archive");
        archive_read_free(a);
        std::cout << "Decompressed successfully to: " << dest_dir << std::endl;
    }
    catch (...)
    {
        archive_read_free(a);
        throw;
    }
}
#endif