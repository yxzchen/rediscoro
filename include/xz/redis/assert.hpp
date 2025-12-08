/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <cassert>

// REDISXZ_ASSERT is disabled by default
// Define REDISXZ_ENABLE_ASSERTS to enable assertions
// This can be controlled via CMake option
#ifdef REDISXZ_ENABLE_ASSERTS
#define REDISXZ_ASSERT(expr) assert(expr)
#else
#define REDISXZ_ASSERT(expr) ((void)0)
#endif
