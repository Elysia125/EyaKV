cmake_minimum_required(VERSION 3.15)
project(EyaKV VERSION 0.1.0 LANGUAGES C CXX)

# 消除 CMake 4.x 关于 FetchContent 的警告
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# gperftools 性能分析开关
option(EYAKV_ENABLE_PROFILER "Enable gperftools (CPU/Heap Profiler)" OFF)

# 设置默认构建类型为 Release（如果不指定）
if(NOT CMAKE_BUILD_TYPE)
    # 开启profiler时自动使用RelWithDebInfo
    if(EYAKV_ENABLE_PROFILER)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    else()
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

# Windows MSVC 编译优化选项
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD /O2 /Ob2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF /INCREMENTAL:NO")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:NONE")
elseif(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC/MinGW 优化选项
    set(RELEASE_FLAGS "")
    # 性能分析模式下禁用 strip 和符号剥离
    if(NOT EYAKV_ENABLE_PROFILER)
        set(RELEASE_FLAGS "${RELEASE_FLAGS} -O3 -s")
        set(RELEASE_FLAGS "${RELEASE_FLAGS} -fdata-sections -ffunction-sections")
    else()
        # 性能分析：仅保留优化，不剥离符号
        set(RELEASE_FLAGS "${RELEASE_FLAGS} -O3 -fno-omit-frame-pointer")
    endif()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${RELEASE_FLAGS}")

    set(SHARED_LINK_FLAGS "")
    if(NOT EYAKV_ENABLE_PROFILER)
        set(SHARED_LINK_FLAGS "${SHARED_LINK_FLAGS} -Wl,--gc-sections -s")
    endif()
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${SHARED_LINK_FLAGS}")
endif()

# 修改动态库前缀（Windows 下去掉 lib 前缀）
if(WIN32)
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
endif()

# Setting build output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include headers globally
include_directories(${PROJECT_SOURCE_DIR}/include)
add_definitions(-DPROJECT_ROOT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

option(EYAKV_STATIC "Build as if using static linkage (disables export macros)" OFF)
if(EYAKV_STATIC)
    add_definitions(-DEYAKV_STATIC)
endif()

# 选项：最小化DLL体积（剥离所有符号）
option(EYAKV_MINIMAL_SIZE "Build minimal size binaries (strip symbols)" ON)
# 性能分析模式下强制关闭最小化体积
if(EYAKV_ENABLE_PROFILER)
    set(EYAKV_MINIMAL_SIZE OFF CACHE BOOL "Build minimal size binaries (strip symbols)" FORCE)
endif()
if(EYAKV_MINIMAL_SIZE)
    if(MSVC)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Os")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:NONE")
    elseif(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Os -s")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -s")
    endif()
endif()

# Define include directory variable for subdirectories
set(EYAKV_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

# Dependencies
include(FetchContent)
enable_testing()

# GTest
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# GLog
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
find_package(glog QUIET)
if (NOT glog_FOUND)
    message(STATUS "GLog not found locally, will attempt to fetch...")
    FetchContent_Declare(
        glog
        URL https://github.com/google/glog/archive/refs/tags/v0.6.0.zip
        CMAKE_ARGS -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF
    )
    FetchContent_MakeAvailable(glog)
endif()

# Spdlog 日志库
message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.zip
)
# 强制 spdlog 编译为静态库（嵌入eyakv_logger 动态库中），不用处理额外的 DLL
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as static" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Build with -fPIC" FORCE)
# 使用 spdlog 自带的内建 fmt 库，避免额外引入依赖
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "Use built-in fmt" FORCE)

FetchContent_MakeAvailable(spdlog)

# LZ4 极速压缩库 (适配 CMake 4.x 极简编译法)
message(STATUS "Fetching lz4...")
FetchContent_Declare(
    lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.9.4.zip
)
FetchContent_MakeAvailable(lz4)

# 绕过 LZ4 过时的 CMake 脚本，直接把它的核心文件编译成静态库
add_library(lz4_static STATIC 
    ${lz4_SOURCE_DIR}/lib/lz4.c
)

# 绑定头文件目录到目标，这样链接它的库能自动拿到 #include <lz4.h> 的路径
target_include_directories(lz4_static PUBLIC 
    $<BUILD_INTERFACE:${lz4_SOURCE_DIR}/lib>
)

# 加上 PIC，并明确指定输出目录，强制对齐全局 CMAKE_ARCHIVE_OUTPUT_DIRECTORY
set_target_properties(lz4_static PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 别忘了在这一段代码的最后，加上 ALIAS
add_library(lz4::lz4_static ALIAS lz4_static)

# 自动查找并链接 gperftools 库
if(EYAKV_ENABLE_PROFILER)
    find_package(Threads REQUIRED)
    find_library(GPERFTOOLS_PROFILER NAMES profiler REQUIRED)
    find_library(GPERFTOOLS_TCMALLOC NAMES tcmalloc REQUIRED)
    message(STATUS "gperftools enabled: Profiler=${GPERFTOOLS_PROFILER}, TCMalloc=${GPERFTOOLS_TCMALLOC}")

    # 全局链接性能库（自动应用到所有子目录的可执行文件/库）
    link_libraries(${GPERFTOOLS_PROFILER} ${GPERFTOOLS_TCMALLOC} Threads::Threads)
endif()

# 子目录
add_subdirectory(src)
add_subdirectory(apps)
add_subdirectory(tests)