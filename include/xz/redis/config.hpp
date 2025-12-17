#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace xz::redis {

enum class resp_version {
  resp2,
  resp3,
};

struct config {
  std::string host = "localhost";
  std::uint16_t port = 6379;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{5000};
  std::string username{};
  std::string password{};
  int database = 0;
  std::string client_name{};
  bool auto_reconnect = true;
  std::size_t max_reconnect_attempts{0};
  std::chrono::milliseconds reconnect_delay{100};
  std::chrono::milliseconds max_reconnect_delay{30000};
  resp_version version = resp_version::resp3;  // Default to RESP3
};

}  // namespace xz::redis
