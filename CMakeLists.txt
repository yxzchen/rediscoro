cmake_minimum_required(VERSION 3.10)
project(redisus VERSION 1.0.0 LANGUAGES CXX)

# Options
option(REDISUS_ENABLE_ASSERTS "Enable assertions in redisus library" OFF)
option(REDISUS_BUILD_TESTS "Build tests" ON)

# Create interface library (header-only)
add_library(redisus INTERFACE)

# Set C++ standard
target_compile_features(redisus INTERFACE cxx_std_20)

# Add include directories
target_include_directories(redisus INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Apply assertion enable flag if option is enabled
if(REDISUS_ENABLE_ASSERTS)
    target_compile_definitions(redisus INTERFACE REDISUS_ENABLE_ASSERTS)
    message(STATUS "Redisus: Assertions are ENABLED")
else()
    message(STATUS "Redisus: Assertions are DISABLED")
endif()

# Installation rules
install(TARGETS redisus
    EXPORT redisus-targets
    INCLUDES DESTINATION include
)

install(DIRECTORY include/
    DESTINATION include
)

install(EXPORT redisus-targets
    FILE redisus-targets.cmake
    NAMESPACE redisus::
    DESTINATION lib/cmake/redisus
)

# Tests
if(REDISUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
