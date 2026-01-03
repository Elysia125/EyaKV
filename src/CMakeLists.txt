# 1. 启用线程支持
find_package(Threads REQUIRED)

# 2. MinGW 环境下，强制使用动态链接的 pthread（避免静态库重复链接）
if(MINGW)
    set(CMAKE_THREAD_PREFER_PTHREAD ON)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()

add_library(eyakv_logger SHARED
    logger/logger.cpp
)
if(WIN32)
    target_compile_definitions(eyakv_logger PRIVATE EYAKV_LOGGER_EXPORTS)
endif()

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

# Module 1: Common - 基础工具库
add_library(eyakv_common SHARED
    common/path_utils.cpp
    common/zset.cpp
    common/bloom_filter.cpp
)

if(WIN32)
    target_compile_definitions(eyakv_common PRIVATE EYAKV_COMMON_EXPORTS)
    target_link_libraries(eyakv_common PRIVATE ${PLATFORM_LIBS} Threads::Threads)
else()
    target_link_libraries(eyakv_common PRIVATE ${PLATFORM_LIBS} Threads::Threads)
endif()

target_include_directories(eyakv_common PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)


# Module 2: Storage - 存储引擎库
add_library(eyakv_storage SHARED
    storage/memtable.cpp
    storage/wal.cpp
    storage/storage.cpp
    storage/sstable.cpp
    storage/processors.cpp
)

if(WIN32)
    target_compile_definitions(eyakv_storage PRIVATE EYAKV_STORAGE_EXPORTS)
    target_link_libraries(eyakv_storage PRIVATE ${PLATFORM_LIBS})
else()
    target_link_libraries(eyakv_storage PRIVATE ${PLATFORM_LIBS})
endif()

target_include_directories(eyakv_storage PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Storage 依赖 Common
target_link_libraries(eyakv_storage PRIVATE eyakv_logger eyakv_common Threads::Threads)



# Module 3: Network - 网络库
add_library(eyakv_network SHARED
    network/server.cpp
)

if(WIN32)
    target_compile_definitions(eyakv_network PRIVATE EYAKV_NETWORK_EXPORTS)
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
    eyakv_storage
    eyakv_common
    Threads::Threads
)

# Module 4: Raft - 共识算法库
add_library(eyakv_raft SHARED
    raft/raft.cpp
)

if(WIN32)
    target_compile_definitions(eyakv_raft PRIVATE EYAKV_RAFT_EXPORTS)
    # target_link_libraries(eyakv_raft PUBLIC ws2_32 mswsock)
endif()

target_include_directories(eyakv_raft PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Raft 依赖 Common
target_link_libraries(eyakv_raft PRIVATE
    eyakv_logger
    eyakv_common
)


# Module 5: Starter - 启动器库
add_library(eyakv_starter SHARED
    starter/starter.cpp
)

if(WIN32)
    target_compile_definitions(eyakv_starter PRIVATE EYAKV_STARTER_EXPORTS)
    # target_link_libraries(eyakv_starter PUBLIC ws2_32 mswsock)
endif()

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
)

# Executables - 可执行文件
add_executable(eyakv_server server.cpp)
target_link_libraries(eyakv_server PRIVATE
    eyakv_logger
    eyakv_storage
    eyakv_network
    eyakv_raft
    eyakv_starter
    eyakv_common
    ${PLATFORM_LIBS}
)

add_executable(eyakv client.cpp)
target_link_libraries(eyakv PRIVATE eyakv_common ${PLATFORM_LIBS})

# Install Rules - 安装规则
install(TARGETS eyakv_server eyakv DESTINATION bin)
install(TARGETS eyakv_logger eyakv_common eyakv_storage eyakv_network eyakv_raft eyakv_starter DESTINATION lib)
install(DIRECTORY ${EYAKV_INCLUDE_DIR} DESTINATION include)