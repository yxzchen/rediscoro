# Find Google Test
find_package(GTest REQUIRED)

add_library(rediscoro_src STATIC rediscoro.cpp)
target_link_libraries(rediscoro_src PUBLIC rediscoro)

macro(make_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  target_link_libraries(${TEST_NAME} PRIVATE
    rediscoro_src
    GTest::gtest
    GTest::gtest_main
  )
  gtest_discover_tests(${TEST_NAME})
endmacro()

make_test(parser_test)
make_test(expected_test)
make_test(adapter_test)
make_test(request_test)
make_test(logger_test)
make_test(connection_test)
make_test(pipeline_test)
