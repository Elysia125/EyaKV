add_executable(eyakv main.cpp)
target_link_libraries(eyakv PRIVATE eyakv_common ${PLATFORM_LIBS})

if(WIN32)
    set_target_properties(eyakv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()
