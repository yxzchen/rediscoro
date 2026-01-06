#pragma once

#include <rediscoro/config.hpp>
#include <rediscoro/detail/connection.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/response.hpp>

#include <iocoro/any_executor.hpp>
#include <iocoro/awaitable.hpp>

#include <memory>

namespace rediscoro {

/// Redis client with coroutine-based async API.
///
/// Responsibilities:
/// - Manage connection lifecycle
/// - Provide user-facing async API
/// - Create and forward requests to connection
///
/// NOT responsible for:
/// - IO operations (delegated to connection)
/// - Protocol parsing (delegated to connection)
/// - Pipeline management (delegated to connection)
///
/// Thread safety:
/// - All methods can be called from any executor
/// - Internally uses connection's strand for serialization
///
/// Usage:
///   client c{ctx.get_executor(), cfg};
///   co_await c.connect();
///   auto result = co_await c.execute<std::string>("GET", "key");
///   co_await c.close();
class client {
public:
  /// Construct a client with the given executor and configuration.
  explicit client(iocoro::any_executor ex, config cfg);

  /// Connect to Redis server.
  /// Performs TCP connection, authentication, and database selection.
  auto connect() -> iocoro::awaitable<void>;

  /// Close the connection gracefully.
  /// Waits for pending requests to complete.
  auto close() -> iocoro::awaitable<void>;

  /// Execute a single command and wait for response.
  ///
  /// Example:
  ///   auto resp = co_await client.execute<std::string>("GET", "mykey");
  ///   if (resp) {
  ///     std::cout << *resp << std::endl;
  ///   }
  template <typename T, typename... Args>
  auto execute(Args&&... args) -> iocoro::awaitable<response_slot<T>>;

  /// Execute a pipeline of commands with compile-time typed responses.
  ///
  /// Example:
  ///   auto [r1, r2] = co_await client.pipeline(
  ///     request{"GET", "key1"},
  ///     request{"SET", "key2", "value"}
  ///   ).execute<std::string, ignore_t>();
  // template <typename... Ts>
  // auto pipeline(...) -> pipeline_builder<Ts...>;

  /// Check if client is connected.
  [[nodiscard]] auto is_connected() const noexcept -> bool;

  /// Get current connection state (for diagnostics).
  [[nodiscard]] auto state() const noexcept -> detail::connection_state;

private:
  std::shared_ptr<detail::connection> conn_;
};

}  // namespace rediscoro

#include <rediscoro/impl/client.ipp>
