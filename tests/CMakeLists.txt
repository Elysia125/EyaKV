include(GoogleTest)

# Define a macro to easily add tests
macro(add_eyakv_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE eyakv_lib gtest gtest_main)
    gtest_discover_tests(${test_name})
endmacro()
