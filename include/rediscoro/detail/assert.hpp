#pragma once

#include <cassert>

namespace rediscoro {

// REDISCORO_ASSERT is disabled by default
// Define REDISCORO_ENABLE_ASSERTS to enable assertions
// This can be controlled via CMake option
#ifdef REDISCORO_ENABLE_ASSERTS

#define REDISCORO_ASSERT_1(cond) assert(cond)
#define REDISCORO_ASSERT_2(cond, msg) assert((cond) && (msg))
#define GET_MACRO(_1, _2, NAME, ...) NAME
#define REDISCORO_ASSERT(...) \
    GET_MACRO(__VA_ARGS__, REDISCORO_ASSERT_2, REDISCORO_ASSERT_1)(__VA_ARGS__)
    
#else
#define REDISCORO_ASSERT(...) ((void)0)
#endif

}  // namespace rediscoro
