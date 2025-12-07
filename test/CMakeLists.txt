# Find Google Test
find_package(GTest REQUIRED)

# Test executable
add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE redisus GTest::gtest GTest::gtest_main)

# Add test
include(GoogleTest)
gtest_discover_tests(parser_test)
