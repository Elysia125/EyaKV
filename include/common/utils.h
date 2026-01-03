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

#endif // UTILS_H