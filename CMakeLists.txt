cmake_minimum_required(VERSION 3.10)
project(redisxz VERSION 1.0.0 LANGUAGES CXX)

# Options
option(REDISXZ_ENABLE_ASSERTS "Enable assertions in redisxz library" OFF)
option(REDISXZ_BUILD_TESTS "Build tests" ON)
option(REDISXZ_USE_STDFMT "Use std::format instead of libfmt" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

# Create interface library (header-only)
add_library(redisxz INTERFACE)

# Set C++ standard
target_compile_features(redisxz INTERFACE cxx_std_20)

# Add include directories
target_include_directories(redisxz INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Fetch external dependencies
include(FetchContent)

FetchContent_Declare(
    ioxz
    GIT_REPOSITORY https://github.com/yxzchen/ioxz.git
    GIT_TAG v0.1.0
)

FetchContent_MakeAvailable(ioxz)

# Link dependencies
target_link_libraries(redisxz INTERFACE ioxz)
target_compile_options(redisxz INTERFACE -fcoroutines)

# Apply assertion enable flag if option is enabled
if(REDISXZ_ENABLE_ASSERTS)
    target_compile_definitions(redisxz INTERFACE REDISXZ_ENABLE_ASSERTS)
    message(STATUS "redisxz: Assertions are ENABLED")
else()
    message(STATUS "redisxz: Assertions are DISABLED")
endif()

# Tests
if(REDISXZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if (ENABLE_ASAN)
  target_compile_options(redisxz INTERFACE
    -g
    -fno-omit-frame-pointer
    -fsanitize=address
  )  
    target_link_options(redisxz INTERFACE
    -fsanitize=address
  )
  message(STATUS "redisxz: AddressSanitizer is ENABLED")
endif()
