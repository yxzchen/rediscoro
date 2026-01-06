#pragma once

#include <chrono>
#include <rediscoro/reconnection_policy.hpp>
#include <string>

namespace rediscoro {

/// Client configuration.
struct config {
  // Connection parameters
  std::string host = "localhost";
  int port = 6379;

  // Timeouts
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{5000};

  // Authentication & setup
  std::string username{};
  std::string password{};
  int database = 0;
  std::string client_name{};

  // Reconnection behavior
  reconnection_policy reconnection{};
};

}  // namespace rediscoro
