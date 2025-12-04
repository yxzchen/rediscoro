cmake_minimum_required(VERSION 3.10)

# Find Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# Enable testing
enable_testing()

# Create test executables
add_executable(parser_test
    parser_test.cpp
)

add_executable(adapter_test
    adapter_test.cpp
)

# Link against redisus and GTest
target_link_libraries(parser_test
    PRIVATE
        redisus
        GTest::GTest
        GTest::Main
)

target_link_libraries(adapter_test
    PRIVATE
        redisus
        GTest::GTest
        GTest::Main
)

# Set C++ standard
target_compile_features(parser_test PRIVATE cxx_std_17)
target_compile_features(adapter_test PRIVATE cxx_std_17)

# Discover tests
gtest_discover_tests(parser_test)
gtest_discover_tests(adapter_test)
