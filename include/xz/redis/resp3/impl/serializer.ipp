/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <xz/redis/resp3/parser.hpp>
#include <xz/redis/resp3/serializer.hpp>

namespace xz::redis::resp3 {

void add_header(std::string& payload, type3 type, std::size_t size) {
  auto const str = std::to_string(size);

  payload += to_code(type);
  payload.append(std::cbegin(str), std::cend(str));
  payload += parser::sep;
}

void add_blob(std::string& payload, std::string_view blob) {
  payload.append(std::cbegin(blob), std::cend(blob));
  payload += parser::sep;
}

void add_separator(std::string& payload) { payload += parser::sep; }

}  // namespace xz::redis::resp3
