/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/adapter/adapter.hpp>
#include <redisus/adapter/adapters.hpp>
#include <redisus/resp3/parser.hpp>

namespace redisus::adapter {

// Convenience function to parse a message into a result using an adapter
template <class T>
auto parse(std::string_view msg, T& result, std::error_code& ec) -> bool {
  resp3::parser p;
  auto adapter = make_adapter(result);
  return resp3::parse(p, msg, adapter, ec);
}

}  // namespace redisus::adapter
