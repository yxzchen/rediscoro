cmake_minimum_required(VERSION 3.15)
project(rediscoro VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(REDISCORO_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(REDISCORO_ENABLE_WARNINGS "Enable warning flags for rediscoro tests/examples" ${PROJECT_IS_TOP_LEVEL})
option(REDISCORO_ENABLE_ASAN "Enable AddressSanitizer" OFF)

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

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(iocoro CONFIG QUIET)
if(NOT iocoro_FOUND)
    FetchContent_Declare(
        iocoro
        GIT_REPOSITORY https://github.com/yxzchen/iocoro.git
        GIT_TAG dev
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
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rediscoroConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rediscoro"
)
