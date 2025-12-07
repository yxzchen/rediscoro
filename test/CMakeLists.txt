# Test executable for partial feeding
add_executable(partial_feeding_test partial_feeding_test.cpp)
target_link_libraries(partial_feeding_test PRIVATE redisus)

# Add test
add_test(NAME partial_feeding_test COMMAND partial_feeding_test)
