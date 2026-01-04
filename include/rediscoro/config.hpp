#pragma once

#include <chrono>
#include <string>

namespace rediscoro {

struct config {
  std::string host = "localhost";
  int port = 6379;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{5000};

  std::string username{};
  std::string password{};
  int database = 0;
  std::string client_name{};

  int immediate_reconnect_attempts{5};
  std::chrono::milliseconds reconnect_delay{100};
  std::chrono::milliseconds max_reconnect_delay{30000};
};

}  // namespace rediscoro
