cmake_minimum_required(VERSION 3.15)
project(EyaKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Setting build output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include headers globally
include_directories(${PROJECT_SOURCE_DIR}/include)
add_definitions(-DPROJECT_ROOT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_definitions(-DEYAKV_STATIC)

# Define include directory variable for subdirectories
set(EYAKV_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
include(FetchContent) 
enable_testing()

# GTest
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent GTest from overriding /MD with /MT
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)


# GLog (Simplified for now, can be replaced/enhanced later)
# We will check if it's installed, otherwise we might skip or fetch.
# For this environment, let's assume we might need to fetch it or just proceed 
# without strictly requiring it for the "Hello World" unless necessary.
# But per plan, we should configure it.
find_package(glog QUIET)
if (NOT glog_FOUND)
    message(STATUS "GLog not found locally, will attempt to fetch...")
    FetchContent_Declare(
        glog
        URL https://github.com/google/glog/archive/refs/tags/v0.6.0.zip
        CMAKE_ARGS -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF
    )
    # On Windows glog might need specific tweaks, keeping it simple for now
    FetchContent_MakeAvailable(glog)
endif()

# ------------------------------------------------------------------------------
# Subdirectories
# ------------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(apps)
add_subdirectory(tests)