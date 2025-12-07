# Find Google Test
find_package(GTest REQUIRED)

add_library(redisus_src STATIC redisus.cpp)
target_link_libraries(redisus_src PUBLIC redisus)

macro(make_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  target_link_libraries(${TEST_NAME} PRIVATE
    redisus_src
    GTest::gtest
    GTest::gtest_main
  )
  gtest_discover_tests(${TEST_NAME})
endmacro()

make_test(parser_test)
