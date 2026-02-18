add_executable(ping ping.cpp)
target_link_libraries(ping PRIVATE rediscoro)

if(TARGET rediscoro_warnings)
  target_link_libraries(ping PRIVATE rediscoro_warnings)
endif()
