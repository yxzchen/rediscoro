# Find Google Test
find_package(GTest REQUIRED)

macro(make_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  target_compile_options(${TEST_NAME} PRIVATE -fcoroutines)
  target_link_libraries(${TEST_NAME} PRIVATE
    rediscoro
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET rediscoro_warnings)
    target_link_libraries(${TEST_NAME} PRIVATE rediscoro_warnings)
  endif()
  gtest_discover_tests(${TEST_NAME})
endmacro()

make_test(adapter_test)
make_test(resp3_types_test)
make_test(resp3_visitor_test)
make_test(resp3_encoder_test)
make_test(resp3_buffer_test)
make_test(resp3_parser_test)
