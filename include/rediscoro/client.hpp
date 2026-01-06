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
///   auto resp = co_await c.exec<std::string>("GET", "key");
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

  /// Execute a request and wait for response(s) (fixed-size, heterogenous).
  ///
  /// For a single command, use Ts... of size 1:
  ///   auto r = co_await client.exec<std::string>("GET", "key");
  ///   auto& slot = r.template get<0>();
  template <typename... Ts>
  auto exec(request req) -> iocoro::awaitable<response<Ts...>>;

  /// Convenience: build a single-command request from args and return response<T>.
  template <typename T, typename... Args>
  auto exec(Args&&... args) -> iocoro::awaitable<response<T>>;

  /// Execute a request and wait for response(s) (dynamic-size, homogeneous).
  template <typename T>
  auto exec_dynamic(request req) -> iocoro::awaitable<dynamic_response<T>>;

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
