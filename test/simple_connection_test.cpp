#include <xz/io/io_context.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection.hpp>

#include <iostream>

int main() {
  std::cout << "Starting simple test\n";

  try {
    xz::redis::config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;

    std::cout << "Creating io_context\n";
    xz::io::io_context ctx;

    std::cout << "Creating connection\n";
    xz::redis::detail::connection conn{ctx, cfg};

    std::cout << "Connection created successfully\n";
  } catch (std::exception const& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  std::cout << "Test passed\n";
  return 0;
}
