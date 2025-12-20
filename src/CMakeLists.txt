# Create the core library
add_library(tinykv_lib
    storage/memtable.cpp
    storage/wal.cpp
    storage/storage.cpp
    storage/sstable.cpp
    network/server.cpp
    network/connection.cpp
)

target_link_libraries(tinykv_lib PUBLIC glog::glog asio::asio)
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    target_link_libraries(tinykv_lib PUBLIC ws2_32 mswsock)
endif()

# Include current directory
target_include_directories(tinykv_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Create the main executable
add_executable(tinykv main.cpp)

# Link the executable to the core library
target_link_libraries(tinykv PRIVATE tinykv_lib)

# Install rules
install(TARGETS tinykv DESTINATION bin)
install(TARGETS tinykv_lib DESTINATION lib)