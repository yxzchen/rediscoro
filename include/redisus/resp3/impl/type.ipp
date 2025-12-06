/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <redisus/resp3/type.hpp>

namespace redisus::resp3 {

auto to_string(type3 type) noexcept -> char const*
{
   switch (type) {
      case type3::array:                return "array";
      case type3::push:                 return "push";
      case type3::set:                  return "set";
      case type3::map:                  return "map";
      case type3::attribute:            return "attribute";
      case type3::simple_string:        return "simple_string";
      case type3::simple_error:         return "simple_error";
      case type3::number:               return "number";
      case type3::doublean:             return "doublean";
      case type3::boolean:              return "boolean";
      case type3::big_number:           return "big_number";
      case type3::null:                 return "null";
      case type3::blob_error:           return "blob_error";
      case type3::verbatim_string:      return "verbatim_string";
      case type3::blob_string:          return "blob_string";
      case type3::streamed_string:      return "streamed_string";
      case type3::streamed_string_part: return "streamed_string_part";
      default:                         return "invalid";
   }
}

auto operator<<(std::ostream& os, type3 type) -> std::ostream&
{
   os << to_string(type);
   return os;
}

}  // namespace redisus::resp3
