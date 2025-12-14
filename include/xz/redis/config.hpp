#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace xz::redis {

struct config {
  std::string host = "localhost";
  std::uint16_t port = 6379;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{5000};
  std::optional<std::string> username;
  std::optional<std::string> password;
  int database = 0;
  std::optional<std::string> client_name;
  bool auto_reconnect = true;
  std::optional<std::size_t> max_reconnect_attempts;
  std::chrono::milliseconds reconnect_delay{100};
  std::chrono::milliseconds max_reconnect_delay{30000};
  bool needs_hello = true;  // RESP3 requires HELLO, RESP2 does not
};

}  // namespace xz::redis
