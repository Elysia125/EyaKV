add_executable(eyakv main.cpp)

add_executable(test_client test_client.cpp)

add_executable(stress_test stress_test.cpp)

# 针对不同平台的链接库
if(WIN32)
    target_link_libraries(eyakv PRIVATE eyakv_common ws2_32)
    set_target_properties(eyakv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    target_link_libraries(test_client PRIVATE eyakv_common ws2_32)
    set_target_properties(test_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    target_link_libraries(stress_test PRIVATE eyakv_common ws2_32)
    set_target_properties(stress_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
else()
    # Linux/Unix 下通常链接 pthread (如果用到多线程)
    target_link_libraries(eyakv PRIVATE eyakv_common pthread)
    target_link_libraries(test_client PRIVATE eyakv_common pthread) 
    target_link_libraries(stress_test PRIVATE eyakv_common pthread)
endif()

