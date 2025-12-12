#pragma once

#include <cassert>

namespace xz::redis {

// REDISXZ_ASSERT is disabled by default
// Define REDISXZ_ENABLE_ASSERTS to enable assertions
// This can be controlled via CMake option
#ifdef REDISXZ_ENABLE_ASSERTS
#define REDISXZ_ASSERT(expr) assert(expr)
#else
#define REDISXZ_ASSERT(expr) ((void)0)
#endif

}  // namespace xz::redis
