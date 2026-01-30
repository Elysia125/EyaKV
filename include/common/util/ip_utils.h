#include <string>
#include <unordered_set>
#include <vector>
#include <utility>
#include "common/util/utils.h"

// 辅助函数：拆分IP和端口（返回：IP部分，端口部分）
// 示例：输入"192.168.1.1:8080" → ("192.168.1.1", "8080")；输入"192.168.*.*" → ("192.168.*.*", "")
static std::pair<std::string, std::string> parse_ip_and_port(const std::string &ip_str)
{
    size_t colon_pos = ip_str.find(':');
    if (colon_pos == std::string::npos)
    {
        return {ip_str, ""}; // 无端口
    }
    std::string ip_part = ip_str.substr(0, colon_pos);
    std::string port_part = ip_str.substr(colon_pos + 1);
    return {ip_part, port_part};
}

// 辅助函数：判断输入IP是否匹配带通配符的规则IP
static bool is_ip_match(const std::string &input_ip, const std::string &rule_ip)
{
    // 拆分IP为四段
    std::vector<std::string> input_segments = split(input_ip, '.');
    std::vector<std::string> rule_segments = split(rule_ip, '.');

    // 基础校验：IP段数必须为4（非法IP直接不匹配）
    if (input_segments.size() != 4 || rule_segments.size() != 4)
    {
        return false;
    }

    // 逐段匹配：* 匹配任意段，否则严格相等
    for (int i = 0; i < 4; ++i)
    {
        if (rule_segments[i] != "*" && rule_segments[i] != input_segments[i])
        {
            return false;
        }
    }
    return true;
}

// 核心函数：判断传入的IP是否受信任
inline bool is_trusted_ip(const std::string &input_ip, const std::unordered_set<std::string> &trusted_set)
{
    // 解析输入IP的IP部分和端口部分
    auto [input_ip_part, input_port_part] = parse_ip_and_port(input_ip);

    // 遍历所有受信任规则，逐一匹配
    for (const std::string &rule : trusted_set)
    {
        auto [rule_ip_part, rule_port_part] = parse_ip_and_port(rule);

        // 第一步：匹配IP部分
        if (!is_ip_match(input_ip_part, rule_ip_part))
        {
            continue;
        }

        // 第二步：匹配端口部分
        // 规则无端口 → 匹配所有端口；规则有端口 → 必须与输入端口完全一致
        if (rule_port_part.empty())
        {
            return true; // IP匹配且规则无端口，直接信任
        }
        else if (rule_port_part == input_port_part)
        {
            return true; // IP和端口都匹配，信任
        }
    }

    // 无任何规则匹配
    return false;
}