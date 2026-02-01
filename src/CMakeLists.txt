# 1. 启用线程支持
find_package(Threads REQUIRED)

# 2. MinGW 环境下，强制使用动态链接的 pthread（避免静态库重复链接）
if(MINGW)
    set(CMAKE_THREAD_PREFER_PTHREAD ON)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()

# Module 0: Logger
add_library(eyakv_logger STATIC
    logger/logger.cpp
)

# Platform-specific settings
if(WIN32)
    # Windows specific settings
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_LIBS ws2_32 mswsock)
elseif(APPLE)
    # macOS specific settings
    set(PLATFORM_LIBS "")
elseif(UNIX)
    # Linux/Unix specific settings
    set(PLATFORM_LIBS pthread)
endif()

# 下载并配置zlib（指定版本，避免兼容性问题）
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1  # 稳定版本，可替换为最新版
    GIT_SHALLOW TRUE
)

# 编译zlib（跳过zlib的测试/示例，加速编译）
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

# 先设置CMP0135策略，彻底消除DOWNLOAD_EXTRACT_TIMESTAMP警告
cmake_policy(SET CMP0135 NEW)

FetchContent_Declare(
  libarchive
  GIT_REPOSITORY https://github.com/libarchive/libarchive.git
  # 使用有效的tag：v3.8.0（3.8系列最新稳定版）
  GIT_TAG v3.8.0  
  # 显式设置下载解压时间戳，配合CMP0135策略消除警告
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  # 浅克隆，加快下载速度
  GIT_SHALLOW TRUE  
)

# 适配 MinGW 构建 libarchive 的配置 
# 禁用不必要的依赖，减少编译问题
set(libarchive_ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_CNG OFF CACHE BOOL "" FORCE) # MinGW不支持Windows CNG
# 额外增加MinGW下的兼容性配置
set(libarchive_ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(libarchive_ENABLE_PCRE2 OFF CACHE BOOL "" FORCE)
set(libarchive_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) # 编译静态库，避免MinGW动态库依赖问题

# 下载并构建 libarchive
FetchContent_MakeAvailable(libarchive)

# 确保能找到libarchive的头文件和库文件
include_directories(${libarchive_SOURCE_DIR}/libarchive)
link_directories(${libarchive_BINARY_DIR})

# Module 1: Common - 基础工具库
add_library(eyakv_common STATIC
    common/util/path_utils.cpp
    common/ds/zset.cpp
    common/ds/bloom_filter.cpp
    common/concurrency/threadpool.cpp
)

if(WIN32)
    target_link_libraries(eyakv_common PRIVATE eyakv_logger ${PLATFORM_LIBS} Threads::Threads)
else()
    target_link_libraries(eyakv_common PRIVATE eyakv_logger ${PLATFORM_LIBS} Threads::Threads)
endif()

target_include_directories(eyakv_common PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)


# Module 2: Storage - 存储引擎库
add_library(eyakv_storage STATIC
    storage/memtable.cpp
    storage/wal.cpp
    storage/storage.cpp
    storage/sstable.cpp
    storage/processors.cpp
)

if(WIN32)
    target_link_libraries(eyakv_storage PRIVATE ${PLATFORM_LIBS})
else()
    target_link_libraries(eyakv_storage PRIVATE ${PLATFORM_LIBS})
endif()

target_include_directories(eyakv_storage PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Storage 依赖 Common
target_link_libraries(eyakv_storage PRIVATE eyakv_logger eyakv_common Threads::Threads archive)



# Module 4: Raft - 共识算法
add_library(eyakv_raft STATIC
    raft/raft.cpp
    raft/raft_log_array.cpp
)

if(WIN32)
    target_link_libraries(eyakv_raft PRIVATE ${PLATFORM_LIBS})
else()
    target_link_libraries(eyakv_raft PRIVATE ${PLATFORM_LIBS})
endif()

target_include_directories(eyakv_raft PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Raft 依赖 Common
target_link_libraries(eyakv_raft PRIVATE
    eyakv_logger
    eyakv_common
    eyakv_storage
    Threads::Threads
    ZLIB::ZLIB
    archive
)

# Module 3: Network - 网络库
add_library(eyakv_network STATIC
    network/tcp_server.cpp
)

if(WIN32)
    target_link_libraries(eyakv_network PUBLIC ${PLATFORM_LIBS})
else()
    target_link_libraries(eyakv_network PUBLIC ${PLATFORM_LIBS})
endif()

target_include_directories(eyakv_network PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Network 依赖 Storage 和 Common
target_link_libraries(eyakv_network PRIVATE
    eyakv_logger
    eyakv_raft
    eyakv_common
    Threads::Threads
)

# Module 5: Starter - 启动器库
add_library(eyakv_starter STATIC
    starter/starter.cpp
)

target_include_directories(eyakv_starter PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Starter 依赖 Storage, Network, Common
target_link_libraries(eyakv_starter PRIVATE
    eyakv_logger
    eyakv_network
    eyakv_common
    eyakv_storage
    eyakv_raft
)

# Executables have been moved to apps/ directory.


# Install Rules - 安装规则
install(TARGETS eyakv_logger eyakv_common eyakv_storage eyakv_network eyakv_raft eyakv_starter DESTINATION lib)
install(DIRECTORY ${EYAKV_INCLUDE_DIR} DESTINATION include)
