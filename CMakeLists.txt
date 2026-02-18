cmake_minimum_required(VERSION 3.15)
project(rediscoro VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(REDISCORO_BUILD_TESTS "Build tests" OFF)
option(REDISCORO_ENABLE_WARNINGS "Enable warning flags for rediscoro tests/examples" ${PROJECT_IS_TOP_LEVEL})
option(REDISCORO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(REDISCORO_ENABLE_COVERAGE "Enable gcov-compatible coverage instrumentation" OFF)
option(REDISCORO_ENABLE_CLANG_TIDY "Enable clang-tidy during compilation" OFF)
option(REDISCORO_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(REDISCORO_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

add_library(rediscoro INTERFACE)
add_library(rediscoro::rediscoro ALIAS rediscoro)
target_include_directories(rediscoro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(rediscoro INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(rediscoro INTERFACE Threads::Threads)

if(REDISCORO_ENABLE_WARNINGS)
    add_library(rediscoro_warnings INTERFACE)
    target_compile_options(rediscoro_warnings INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
    )
endif()

if(REDISCORO_ENABLE_ASAN)
    if(REDISCORO_ENABLE_COVERAGE)
        message(FATAL_ERROR "REDISCORO_ENABLE_ASAN and REDISCORO_ENABLE_COVERAGE cannot be enabled together")
    endif()
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

if(REDISCORO_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(rediscoro INTERFACE
            -O0
            -g
            --coverage
        )
        target_link_options(rediscoro INTERFACE
            --coverage
        )
        message(STATUS "rediscoro: Coverage instrumentation is ENABLED")
    else()
        message(FATAL_ERROR "REDISCORO_ENABLE_COVERAGE requires GCC/Clang-compatible compiler")
    endif()
endif()

if(REDISCORO_ENABLE_CLANG_TIDY)
    find_program(REDISCORO_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17)
    if(NOT REDISCORO_CLANG_TIDY_EXE)
        message(FATAL_ERROR "REDISCORO_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
    endif()
    set(CMAKE_CXX_CLANG_TIDY "${REDISCORO_CLANG_TIDY_EXE};--use-color;--quiet")
    message(STATUS "rediscoro: clang-tidy is ENABLED (${REDISCORO_CLANG_TIDY_EXE})")
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(iocoro CONFIG QUIET)
if(NOT iocoro_FOUND)
    FetchContent_Declare(
        iocoro
        GIT_REPOSITORY https://github.com/yxzchen/iocoro.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(iocoro)
endif()
target_link_libraries(rediscoro INTERFACE iocoro::iocoro)

# Some toolchains still require an explicit coroutines flag even in C++20 mode.
target_compile_options(rediscoro INTERFACE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>
)

if(REDISCORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(REDISCORO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(REDISCORO_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

install(TARGETS rediscoro EXPORT rediscoroTargets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT rediscoroTargets
    NAMESPACE rediscoro::
    FILE rediscoroTargets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rediscoro"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rediscoroConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rediscoro"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ExactVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rediscoro"
)
