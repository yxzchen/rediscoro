#pragma once

#include <rediscoro/config.hpp>
#include <rediscoro/detail/connection.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/response.hpp>

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>

#include <memory>
#include <optional>
#include <utility>

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
  explicit client(iocoro::any_io_executor ex, config cfg)
      : conn_(std::make_shared<detail::connection>(ex, std::move(cfg))) {}

  /// Connect to Redis server.
  /// Performs TCP connection, authentication, and database selection.
  ///
  /// Returns:
  /// - expected<void, error_info>{} on success
  /// - unexpected(error_info) with error details on failure
  auto connect() -> iocoro::awaitable<expected<void, error_info>> {
    co_return co_await conn_->connect();
  }

  /// Close the connection gracefully.
  /// Waits for pending requests to complete.
  auto close() -> iocoro::awaitable<void> { co_return co_await conn_->close(); }

  /// Execute a request and wait for response(s) (fixed-size, heterogenous).
  ///
  /// For a single command, use Ts... of size 1:
  ///   auto r = co_await client.exec<std::string>("GET", "key");
  ///   auto& slot = r.get<0>();
  template <typename... Ts>
  auto exec(request req) -> iocoro::awaitable<response<Ts...>> {
    auto pending = conn_->enqueue<Ts...>(std::move(req));
    co_return co_await pending->wait();
  }

  /// Convenience: build a single-command request from args and return response<T>.
  template <typename T, typename... Args>
  auto exec(Args&&... args) -> iocoro::awaitable<response<T>> {
    request req{std::forward<Args>(args)...};
    auto pending = conn_->enqueue<T>(std::move(req));
    co_return co_await pending->wait();
  }

  /// Execute a request and wait for response(s) (dynamic-size, homogeneous).
  template <typename T>
  auto exec_dynamic(request req) -> iocoro::awaitable<dynamic_response<T>> {
    auto pending = conn_->enqueue_dynamic<T>(std::move(req));
    co_return co_await pending->wait();
  }

  /// Check if client is connected.
  [[nodiscard]] bool is_connected() const noexcept {
    return conn_->state() == detail::connection_state::OPEN;
  }

  /// Get current connection state (for diagnostics).
  [[nodiscard]] auto state() const noexcept -> detail::connection_state { return conn_->state(); }

  /// Get the last runtime connection error (for diagnostics).
  [[nodiscard]] auto last_error() const noexcept -> std::optional<error_info> {
    return conn_->last_error();
  }

 private:
  std::shared_ptr<detail::connection> conn_;
};

}  // namespace rediscoro
