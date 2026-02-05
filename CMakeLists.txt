cmake_minimum_required(VERSION 3.15)
project(EyaKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置默认构建类型为 Release（如果不指定）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Windows MSVC 编译优化选项
if(MSVC)
    # 使用多线程DLL运行时 (/MD)，避免静态链接
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD /O2 /Ob2 /DNDEBUG")
    # 移除调试符号，减小体积
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /Gy")
    # 链接器优化
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF /INCREMENTAL:NO")
elseif(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC/MinGW 优化选项
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -s")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fdata-sections -ffunction-sections")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -Wl,--gc-sections -s")
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

# 默认构建为共享库（src 下的各模块都是 SHARED），不应强制定义 EYAKV_STATIC。
# EYAKV_STATIC 一旦定义，会让 export 宏为空，配合 -fvisibility=hidden 会导致 Linux 下符号不可见而链接失败。
option(EYAKV_STATIC "Build as if using static linkage (disables export macros)" OFF)
if(EYAKV_STATIC)
    add_definitions(-DEYAKV_STATIC)
endif()

# 选项：最小化DLL体积（剥离所有符号）
option(EYAKV_MINIMAL_SIZE "Build minimal size binaries (strip symbols)" OFF)
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

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
include(FetchContent) 
enable_testing()

# GTest
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent GTest from overriding /MD with /MT
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)


# GLog (Simplified for now, can be replaced/enhanced later)
# We will check if it's installed, otherwise we might skip or fetch.
# For this environment, let's assume we might need to fetch it or just proceed 
# without strictly requiring it for the "Hello World" unless necessary.
# But per plan, we should configure it.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)     # 告诉 glog 不要去依赖 gtest
find_package(glog QUIET)
if (NOT glog_FOUND)
    message(STATUS "GLog not found locally, will attempt to fetch...")
    FetchContent_Declare(
        glog
        URL https://github.com/google/glog/archive/refs/tags/v0.6.0.zip
        CMAKE_ARGS -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF
    )
    # On Windows glog might need specific tweaks, keeping it simple for now
    FetchContent_MakeAvailable(glog)
endif()

# 子目录
add_subdirectory(src)
add_subdirectory(apps)
add_subdirectory(tests)