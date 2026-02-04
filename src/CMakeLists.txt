# 1. 启用线程支持
find_package(Threads REQUIRED)

# 2. MinGW 环境强制动态 pthread + 符号隐藏（关键修复）
if(MINGW)
    set(CMAKE_THREAD_PREFER_PTHREAD ON)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    # 强制链接动态 pthread 库（libpthread-1.dll）
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    # 编译选项：启用 pthread + 隐藏非导出符号（避免导出 pthread 符号）
    add_compile_options(-pthread -fvisibility=hidden)
    # 链接选项：强制动态链接 + 排除静态 pthread 库 + 隐藏冗余符号
    add_link_options(
        -Wl,-Bdynamic  # 优先动态链接
        -lpthread      # 动态 pthread 库
        -Wl,--exclude-libs,libpthread.a  # 禁止链接静态 pthread
        -Wl,--as-needed  # 只链接必要的库
    )
endif()

# Module 0: Logger（必须显式链接线程库，避免隐式静态链接）
add_library(eyakv_logger SHARED
    logger/logger.cpp
)
target_compile_definitions(eyakv_logger
    PRIVATE
        _GNU_SOURCE
        _POSIX_C_SOURCE=200809L
)
# 平台相关配置
if(WIN32)
    target_compile_definitions(eyakv_logger PRIVATE EYAKV_LOGGER_EXPORTS)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_LIBS ws2_32 mswsock)
elseif(APPLE)
    set(PLATFORM_LIBS "")
elseif(UNIX)
    set(PLATFORM_LIBS "")  # Threads::Threads 已包含 pthread，无需冗余
endif()
# 关键修复：给 Logger 显式链接线程库（强制动态）
target_link_libraries(eyakv_logger PRIVATE
    ${PLATFORM_LIBS}
    Threads::Threads
)
# 仅导出 EYAKV_LOGGER_EXPORTS 标记的符号（隐藏 pthread 符号）
set_target_properties(eyakv_logger PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Module 1: Common（无需修改链接逻辑，继承动态 pthread）
add_library(eyakv_common SHARED
    common/util/path_utils.cpp
    common/ds/zset.cpp
    common/ds/bloom_filter.cpp
    common/concurrency/threadpool.cpp
)
if(WIN32)
    target_compile_definitions(eyakv_common PRIVATE EYAKV_COMMON_EXPORTS)
    target_link_libraries(eyakv_common PRIVATE eyakv_logger ${PLATFORM_LIBS} Threads::Threads)
else()
    target_link_libraries(eyakv_common PRIVATE eyakv_logger ${PLATFORM_LIBS} Threads::Threads)
endif()
target_include_directories(eyakv_common PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
# 隐藏 Common 的非导出符号（可选，增强安全性）
set_target_properties(eyakv_common PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# 后续模块（Storage/Raft/Network/Starter）保持原样不变！
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
target_link_libraries(eyakv_storage PRIVATE eyakv_logger eyakv_common Threads::Threads)
set_target_properties(eyakv_storage PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Module 4: Raft - 共识算法
add_library(eyakv_raft SHARED
    raft/raft.cpp
    raft/raft_log_array.cpp
)
if(WIN32)
    target_compile_definitions(eyakv_raft PRIVATE EYAKV_RAFT_EXPORTS)
    target_link_libraries(eyakv_raft PRIVATE ${PLATFORM_LIBS})
else()
    target_link_libraries(eyakv_raft PRIVATE ${PLATFORM_LIBS})
endif()
target_include_directories(eyakv_raft PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(eyakv_raft PRIVATE
    eyakv_logger
    eyakv_common
    eyakv_storage
    Threads::Threads
)
set_target_properties(eyakv_raft PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Module 3: Network - 网络库
add_library(eyakv_network SHARED
    network/tcp_server.cpp
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
target_link_libraries(eyakv_network PRIVATE
    eyakv_logger
    eyakv_raft
    eyakv_common
    Threads::Threads
)
set_target_properties(eyakv_network PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Module 5: Starter - 启动器库
add_library(eyakv_starter SHARED
    starter/starter.cpp
)
if(WIN32)
    target_compile_definitions(eyakv_starter PRIVATE EYAKV_STARTER_EXPORTS)
endif()
target_include_directories(eyakv_starter PUBLIC
    ${EYAKV_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(eyakv_starter PRIVATE
    eyakv_logger
    eyakv_network
    eyakv_common
    eyakv_storage
    eyakv_raft
)
set_target_properties(eyakv_starter PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Install Rules - 安装规则
install(TARGETS eyakv_logger eyakv_common eyakv_storage eyakv_network eyakv_raft eyakv_starter DESTINATION lib)
install(DIRECTORY ${EYAKV_INCLUDE_DIR} DESTINATION include)