#ifndef UTILS_H
#define UTILS_H

#include <string>
inline int compare_double_strings(const std::string &a, const std::string &b)
{
    std::string a_clean = a.substr(0, a.find('\0'));
    std::string b_clean = b.substr(0, b.find('\0'));
    double da = std::stod(a_clean);
    double db = std::stod(b_clean);
    return (da < db) ? -1 : ((da > db) ? 1 : 0);
}
/**
 * @brief 计算 std::string 的实际大小
 */
inline size_t calculateStringSize(const std::string &str)
{
    return str.size() + sizeof(std::string);
}

inline std::string generate_general_key(size_t key_length)
{
    // 校验输入合法性
    if (key_length == 0)
    {
        throw std::invalid_argument("密钥长度不能为0");
    }

    // 定义密钥字符集：大小写字母 + 数字 + 常用符号
    const std::string charset =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "!@#$%^&*()_+-=[]{}|;:,.<>?";

    // 初始化高质量随机数生成器
    std::random_device rd;                                        // 获取真随机数（系统支持的话）作为种子
    std::mt19937 gen(rd());                                       // 梅森旋转算法，随机质量高、效率好
    std::uniform_int_distribution<> distr(0, charset.size() - 1); // 均匀分布

    std::string key;
    key.reserve(key_length); // 预分配内存，提升效率

    // 循环生成密钥字符
    for (size_t i = 0; i < key_length; ++i)
    {
        key += charset[distr(gen)];
    }

    return key;
}

/**
 * @brief 分割字符串（单个分隔符）
 * @param str 待分割的原字符串
 * @param delimiter 分隔符（单个字符）
 * @return 分割后的字符串集合
 */
std::vector<std::string> split(const std::string &str, char delimiter)
{
    std::vector<std::string> result;
    std::string current_substr; // 存储当前截取的子串

    // 遍历原字符串的每个字符
    for (char c : str)
    {
        if (c == delimiter)
        {
            // 遇到分隔符：将当前子串加入结果，然后清空
            result.push_back(current_substr);
            current_substr.clear();
        }
        else
        {
            // 非分隔符：追加到当前子串
            current_substr += c;
        }
    }

    // 处理最后一段子串（原字符串末尾没有分隔符的情况）
    result.push_back(current_substr);

    return result;
}
#endif // UTILS_H