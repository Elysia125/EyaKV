# Create the core library
add_library(eyakv_lib
    storage/memtable.cpp
    storage/wal.cpp
    storage/storage.cpp
    storage/sstable.cpp
    network/server.cpp
    common/path_utils.cpp
    common/zset.cpp
    starter/starter.cpp
    storage/processors.cpp
)

# target_link_libraries(eyakv_lib PUBLIC glog::glog asio::asio)
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    target_link_libraries(eyakv_lib PUBLIC ws2_32 mswsock)
endif()

# Include current directory
target_include_directories(eyakv_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Create the main executable
add_executable(eyakv_server server.cpp)
# add_executable(eyakv client.cpp)
# Link the executable to the core library
target_link_libraries(eyakv_server PRIVATE eyakv_lib)


# Install rules
install(TARGETS eyakv_server DESTINATION bin)
install(TARGETS eyakv_lib DESTINATION lib)