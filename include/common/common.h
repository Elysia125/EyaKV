#pragma once

#include <string>

namespace tinykv {

inline constexpr auto PROJECT_NAME = "TinyKV";

// Simple utility function
inline std::string build_info() {
    return std::string(PROJECT_NAME) + " v0.1.0";
}

} // namespace tinykv
