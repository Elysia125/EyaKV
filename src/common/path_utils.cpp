#include "common/path_utils.h"

// 定义静态成员
std::optional<std::string> PathUtils::exe_dir_;
std::mutex PathUtils::cache_mutex_;