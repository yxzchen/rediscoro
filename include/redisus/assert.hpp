/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <cassert>

// REDISUS_ASSERT is disabled by default
// Define REDISUS_ENABLE_ASSERTS to enable assertions
// This can be controlled via CMake option
#ifdef REDISUS_ENABLE_ASSERTS
#define REDISUS_ASSERT(expr) assert(expr)
#else
#define REDISUS_ASSERT(expr) ((void)0)
#endif
