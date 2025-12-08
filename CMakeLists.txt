cmake_minimum_required(VERSION 3.10)
project(redisxz VERSION 1.0.0 LANGUAGES CXX)

# Options
option(REDISXZ_ENABLE_ASSERTS "Enable assertions in redisxz library" OFF)
option(REDISXZ_BUILD_TESTS "Build tests" ON)

# Create interface library (header-only)
add_library(redisxz INTERFACE)

# Set C++ standard
target_compile_features(redisxz INTERFACE cxx_std_20)

# Add include directories
target_include_directories(redisxz INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Apply assertion enable flag if option is enabled
if(REDISXZ_ENABLE_ASSERTS)
    target_compile_definitions(redisxz INTERFACE REDISXZ_ENABLE_ASSERTS)
    message(STATUS "redisxz: Assertions are ENABLED")
else()
    message(STATUS "redisxz: Assertions are DISABLED")
endif()

# Tests
if(REDISXZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
