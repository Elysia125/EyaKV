add_executable(eyakv main.cpp)
target_link_libraries(eyakv PRIVATE eyakv_common ${PLATFORM_LIBS})

add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE eyakv_common)

add_executable(stress_test stress_test.cpp)
target_link_libraries(stress_test PRIVATE eyakv_common)

# 针对不同平台的链接库
if(WIN32)
    # Windows 下链接 ws2_32
    target_link_libraries(test_client PRIVATE ws2_32)
    target_link_libraries(stress_test PRIVATE ws2_32)
    set_target_properties(eyakv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    set_target_properties(test_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
else()
    # Linux/Unix 下通常链接 pthread (如果用到多线程)
    target_link_libraries(test_client PRIVATE pthread) 
    target_link_libraries(stress_test PRIVATE pthread)
endif()

