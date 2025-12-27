cmake_minimum_required(VERSION 3.10)
project(rediscoro VERSION 1.0.0 LANGUAGES CXX)

# Options
option(REDISCORO_ENABLE_ASSERTS "Enable assertions in rediscoro library" OFF)
option(REDISCORO_BUILD_TESTS "Build tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

# Create interface library (header-only)
add_library(rediscoro INTERFACE)

# Set C++ standard
target_compile_features(rediscoro INTERFACE cxx_std_20)

# Add include directories
target_include_directories(rediscoro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Fetch external dependencies
include(FetchContent)

# CI / offline-friendly builds: do not try to update FetchContent deps from the network
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    iocoro
    GIT_REPOSITORY https://github.com/yxzchen/iocoro.git
    GIT_TAG archive/0.2.0
)

FetchContent_MakeAvailable(iocoro)

# Link dependencies
target_link_libraries(rediscoro INTERFACE ioxz)
target_compile_options(rediscoro INTERFACE -fcoroutines)

# Apply assertion enable flag if option is enabled
if(REDISCORO_ENABLE_ASSERTS)
    target_compile_definitions(rediscoro INTERFACE REDISCORO_ENABLE_ASSERTS)
    message(STATUS "rediscoro: Assertions are ENABLED")
else()
    message(STATUS "rediscoro: Assertions are DISABLED")
endif()

# Tests
if(REDISCORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if (ENABLE_ASAN)
  target_compile_options(rediscoro INTERFACE
    -g
    -fno-omit-frame-pointer
    -fsanitize=address
  )  
    target_link_options(rediscoro INTERFACE
    -fsanitize=address
  )
  message(STATUS "rediscoro: AddressSanitizer is ENABLED")
endif()
