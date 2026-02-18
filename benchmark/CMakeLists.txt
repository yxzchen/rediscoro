include(FetchContent)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

FetchContent_Declare(
  boost_189
  URL https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(boost_189)

function(add_rediscoro_benchmark name)
  add_executable(rediscoro_${name} cases/rediscoro/${name}.cpp)
  target_link_libraries(rediscoro_${name} PRIVATE rediscoro Threads::Threads)
  target_compile_features(rediscoro_${name} PRIVATE cxx_std_20)
  target_compile_options(rediscoro_${name} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
  if(TARGET rediscoro_warnings)
    target_link_libraries(rediscoro_${name} PRIVATE rediscoro_warnings)
  endif()
endfunction()

function(add_boostredis_benchmark name)
  add_executable(boostredis_${name} cases/boostredis/${name}.cpp)
  target_link_libraries(boostredis_${name} PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
  target_compile_features(boostredis_${name} PRIVATE cxx_std_20)
  target_compile_options(boostredis_${name} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
  target_include_directories(boostredis_${name} PRIVATE ${boost_189_SOURCE_DIR})
endfunction()

set(BENCHMARK_NAMES
  redis_latency
  redis_throughput
  redis_pipeline_depth
)

foreach(bench_name IN LISTS BENCHMARK_NAMES)
  add_rediscoro_benchmark(${bench_name})
  add_boostredis_benchmark(${bench_name})
endforeach()
