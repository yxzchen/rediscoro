/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/resp3/parser.hpp>
#include <redisus/resp3/type.hpp>

#include <redisus/assert.hpp>

#include <string>
#include <tuple>

namespace redisus::resp3 {

void add_header(std::string& payload, type_t type, std::size_t size);
void add_blob(std::string& payload, std::string_view blob);
void add_separator(std::string& payload);

}  // namespace redisus::resp3
