#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/resp3/builder.hpp>

#include <iocoro/this_coro.hpp>

#include <span>

namespace rediscoro::detail {

inline auto connection::do_read() -> iocoro::awaitable<void> {
  if (state_ != connection_state::OPEN) {
    co_return;
  }

  struct in_flight_guard {
    bool& flag;
    explicit in_flight_guard(bool& f) : flag(f) {
      REDISCORO_ASSERT(!flag, "concurrent read detected");
      flag = true;
    }
    ~in_flight_guard() { flag = false; }
  };

  in_flight_guard guard{read_in_flight_};

  // Socket-driven read: perform one read operation (may parse multiple messages from the buffer).
  // This allows detecting peer close even when no pending_read exists.
  auto writable = parser_.prepare();
  auto r = co_await socket_.async_read_some(writable);
  if (!r) {
    // Socket IO error - treat as connection lost
    handle_error(client_errc::connection_lost);
    co_return;
  }

  if (*r == 0) {
    // Peer closed (EOF).
    handle_error(client_errc::connection_reset);
    co_return;
  }

  parser_.commit(*r);

  for (;;) {
    auto parsed = parser_.parse_one();
    if (!parsed) {
      // Deliver parser error into the pipeline, then treat it as a fatal connection error.
      if (pipeline_.has_pending_read()) {
        pipeline_.on_error(parsed.error());
      }
      handle_error(parsed.error());
      co_return;
    }
    if (!parsed->has_value()) {
      break;
    }

    if (!pipeline_.has_pending_read()) {
      // Unsolicited message (e.g. PUSH) is not supported yet.
      // Temporary policy: treat as "unsupported feature" rather than protocol violation.
      handle_error(client_errc::unsolicited_message);
      co_return;
    }

    auto const root = **parsed;
    auto msg = resp3::build_message(parser_.tree(), root);
    pipeline_.on_message(std::move(msg));

    // Critical for zero-copy parser: reclaim before parsing the next message.
    parser_.reclaim();
  }

  co_return;
}

inline auto connection::do_write() -> iocoro::awaitable<void> {
  if (state_ != connection_state::OPEN) {
    co_return;
  }

  struct in_flight_guard {
    bool& flag;
    explicit in_flight_guard(bool& f) : flag(f) {
      REDISCORO_ASSERT(!flag, "concurrent write detected");
      flag = true;
    }
    ~in_flight_guard() { flag = false; }
  };

  in_flight_guard guard{write_in_flight_};

  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ == connection_state::OPEN &&
         pipeline_.has_pending_write()) {
    auto view = pipeline_.next_write_buffer();
    auto buf = std::as_bytes(std::span{view.data(), view.size()});

    auto r = co_await socket_.async_write_some(buf);
    if (!r) {
      handle_error(client_errc::write_error);
      co_return;
    }

    pipeline_.on_write_done(*r);
    if (pipeline_.has_pending_read()) {
      read_wakeup_.notify();
    }
  }

  co_return;
}

inline auto connection::handle_error(error_info ec) -> void {
  // Centralized runtime error path:
  // - Only OPEN may transition to FAILED (runtime IO errors after first OPEN).
  // - CONNECTING/INIT errors are handled by do_connect()/connect() and must not enter FAILED.
  // - Must NOT write CLOSED (only transition_to_closed()).

  if (state_ == connection_state::CLOSED || state_ == connection_state::CLOSING) {
    return;
  }

  if (state_ == connection_state::FAILED || state_ == connection_state::RECONNECTING) {
    return;
  }

  REDISCORO_ASSERT(state_ == connection_state::OPEN,
                   "handle_error must not be used for CONNECTING/INIT errors");
  if (state_ != connection_state::OPEN) {
    // Non-OPEN state errors are not recorded in last_error_ (handled elsewhere).
    control_wakeup_.notify();
    return;
  }

  // OPEN runtime error -> FAILED.
  // Record error for diagnostics (user cannot obtain it directly).
  auto const err = ec;
  set_last_error(std::move(ec));
  state_ = connection_state::FAILED;
  pipeline_.clear_all(err);
  if (socket_.is_open()) {
    (void)socket_.close();
  }
  control_wakeup_.notify();
  write_wakeup_.notify();
  read_wakeup_.notify();
}

}  // namespace rediscoro::detail
