# Find Google Test
find_package(GTest REQUIRED)

macro(make_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  target_link_libraries(${TEST_NAME} PRIVATE
    rediscoro
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
