include(GoogleTest)

# Define a macro to easily add tests
macro(add_tinykv_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE tinykv_lib gtest gtest_main)
    gtest_discover_tests(${test_name})
endmacro()

# Add tests
add_tinykv_test(test_memtable test_memtable.cpp)
add_tinykv_test(test_storage test_storage.cpp)

# Network test (needs to link asio)
add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE tinykv_lib gtest gtest_main asio::asio)
gtest_discover_tests(test_client)