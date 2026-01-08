add_executable(eyakv main.cpp)
target_link_libraries(eyakv PRIVATE eyakv_common ${PLATFORM_LIBS})

add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE eyakv_common ws2_32)

add_executable(stress_test stress_test.cpp)
target_link_libraries(stress_test PRIVATE eyakv_common ws2_32)

if(WIN32)
    set_target_properties(eyakv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    set_target_properties(test_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()
