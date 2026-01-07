add_executable(eyakv_server main.cpp)
target_link_libraries(eyakv_server PRIVATE
    eyakv_logger
    eyakv_storage
    eyakv_network
    eyakv_raft
    eyakv_starter
    eyakv_common
    ${PLATFORM_LIBS}
)

if(WIN32)
    set_target_properties(eyakv_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()
