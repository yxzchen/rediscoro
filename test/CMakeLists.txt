cmake_minimum_required(VERSION 3.10)

# Find Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# Enable testing
enable_testing()

# Create test executable
add_executable(parser_test
    parser_test.cpp
)

# Link against redisus and GTest
target_link_libraries(parser_test
    PRIVATE
        redisus
        GTest::GTest
        GTest::Main
)

# Set C++ standard
target_compile_features(parser_test PRIVATE cxx_std_17)

# Discover tests
gtest_discover_tests(parser_test)
