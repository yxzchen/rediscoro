/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <redisus/resp3/type.hpp>

namespace redisus::resp3 {

auto to_string(type_t type) noexcept -> char const*
{
   switch (type) {
      case type_t::array:                return "array";
      case type_t::push:                 return "push";
      case type_t::set:                  return "set";
      case type_t::map:                  return "map";
      case type_t::attribute:            return "attribute";
      case type_t::simple_string:        return "simple_string";
      case type_t::simple_error:         return "simple_error";
      case type_t::number:               return "number";
      case type_t::doublean:             return "doublean";
      case type_t::boolean:              return "boolean";
      case type_t::big_number:           return "big_number";
      case type_t::null:                 return "null";
      case type_t::blob_error:           return "blob_error";
      case type_t::verbatim_string:      return "verbatim_string";
      case type_t::blob_string:          return "blob_string";
      case type_t::streamed_string:      return "streamed_string";
      case type_t::streamed_string_part: return "streamed_string_part";
      default:                         return "invalid";
   }
}

auto operator<<(std::ostream& os, type_t type) -> std::ostream&
{
   os << to_string(type);
   return os;
}

}  // namespace redisus::resp3
