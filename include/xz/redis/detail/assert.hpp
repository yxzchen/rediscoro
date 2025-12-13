#pragma once

#include <cassert>

namespace xz::redis {

// REDISXZ_ASSERT is disabled by default
// Define REDISXZ_ENABLE_ASSERTS to enable assertions
// This can be controlled via CMake option
#ifdef REDISXZ_ENABLE_ASSERTS

#define REDISXZ_ASSERT_1(cond) assert(cond)
#define REDISXZ_ASSERT_2(cond, msg) assert((cond) && (msg))
#define GET_MACRO(_1, _2, NAME, ...) NAME
#define REDISXZ_ASSERT(...) \
    GET_MACRO(__VA_ARGS__, REDISXZ_ASSERT_2, REDISXZ_ASSERT_1)(__VA_ARGS__)
    
#else
#define REDISXZ_ASSERT(...) ((void)0)
#endif

}  // namespace xz::redis
