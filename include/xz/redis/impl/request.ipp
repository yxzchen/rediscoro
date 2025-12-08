#include <xz/redis/request.hpp>

#include <string_view>

namespace xz::redis::detail {

auto is_subscribe(std::string_view cmd) -> bool
{
   if (cmd == "SUBSCRIBE")
      return true;
   if (cmd == "PSUBSCRIBE")
      return true;
   if (cmd == "UNSUBSCRIBE")
      return true;
   if (cmd == "PUNSUBSCRIBE")
      return true;
   return false;
}

}  // namespace xz::redis::detail
