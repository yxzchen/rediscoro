#include <rediscoro/request.hpp>

#include <string_view>

namespace rediscoro::detail {

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

}  // namespace rediscoro::detail
