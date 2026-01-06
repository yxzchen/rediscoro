// Basic compilation test for client framework

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

auto main() -> int {
  // Basic compilation test
  iocoro::io_context ctx;

  rediscoro::config cfg;
  cfg.host = "localhost";
  cfg.port = 6379;

  rediscoro::client client{ctx.get_executor(), cfg};

  // This is just a compilation test
  // Actual implementation will be done later

  return 0;
}
