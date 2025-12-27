#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <rediscoro/adapter/any_adapter.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/detail/connection_impl.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/response.hpp>

#include <memory>
#include <system_error>
#include <tuple>

namespace rediscoro {

namespace detail {
template <class... Ts>
struct execute_result;

template <class T>
struct execute_result<T> {
  using type = response0<T>;
};

template <class T1, class T2, class... Ts>
struct execute_result<T1, T2, Ts...> {
  using type = response<T1, T2, Ts...>;
};
}

/**
 * @brief Lightweight RAII handle to a Redis connection
 *
 * - Destructor calls `stop()` but does NOT block
 * - Background tasks (read_loop, reconnect_loop) are kept alive by shared impl state
 * - For graceful shutdown that waits for tasks, call `graceful_stop()` before destruction
 *
 * Thread Safety:
 * - connection is NOT thread-safe
 * - All public methods must be called from the same io_context thread
 */
class connection {
 public:
  using state = detail::connection_impl::state;

  connection(iocoro::executor ex, config cfg);
  connection(iocoro::io_context& ctx, config cfg) : connection(ctx.get_executor(), std::move(cfg)) {}
  ~connection();

  connection(connection const&) = delete;
  auto operator=(connection const&) -> connection& = delete;
  connection(connection&&) = default;
  auto operator=(connection&&) -> connection& = default;

  /**
   * @brief Start the connection (TCP connect + handshake + read loop)
   */
  auto run() -> iocoro::awaitable<void>;

  /// Execute a request and adapt its responses into `resp`.
  template <class Response>
  auto execute(request const& req, Response& resp) -> iocoro::awaitable<void> {
    co_await impl_->execute_any(req, adapter::any_adapter{resp});
  }

  /// Execute a request and return its adapted reply object by value.
  ///
  /// - For one reply type: returns `response0<T>`
  /// - For multiple reply types: returns `response<Ts...>`
  template <class... Ts>
  auto execute_one(request const& req)
    -> iocoro::awaitable<typename detail::execute_result<Ts...>::type> {
    static_assert(sizeof...(Ts) > 0, "execute_one<Ts...> requires at least one reply type");
    typename detail::execute_result<Ts...>::type resp{};
    co_await execute(req, resp);
    co_return resp;
  }

  /// Stop the connection (non-blocking).
  ///
  /// Background tasks will exit soon, but this returns immediately.
  void stop();

  /// Stop and wait for all background tasks to complete.
  ///
  /// Use this before destruction if you need guaranteed cleanup.
  auto graceful_stop() -> iocoro::awaitable<void>;

  [[nodiscard]] auto current_state() const noexcept -> state;
  [[nodiscard]] auto is_running() const noexcept -> bool;
  auto error() const -> std::error_code;
  auto get_executor() noexcept -> iocoro::executor;

 private:
  std::shared_ptr<detail::connection_impl> impl_;
};

}  // namespace rediscoro
