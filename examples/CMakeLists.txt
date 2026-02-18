set(REDISCORO_EXAMPLES
  ping
  static_response
  dynamic_response
  pipeline_mixed_results
  adapter_containers
  request_timeout
  tracing_hooks
)

foreach(example IN LISTS REDISCORO_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE rediscoro)

  if(TARGET rediscoro_warnings)
    target_link_libraries(${example} PRIVATE rediscoro_warnings)
  endif()
endforeach()
