add_executable(ping_example ping.cpp)
target_link_libraries(ping_example PRIVATE rediscoro)

if(TARGET rediscoro_warnings)
  target_link_libraries(ping_example PRIVATE rediscoro_warnings)
endif()

