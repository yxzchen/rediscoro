#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/builder.hpp>

#include <iocoro/ip/resolver.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/with_timeout.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <system_error>

namespace rediscoro::detail {

inline auto connection::do_connect() -> iocoro::awaitable<expected<void, error_info>> {
  auto tok = co_await iocoro::this_coro::stop_token;
  if (tok.stop_requested()) {
    REDISCORO_LOG_WARNING("connect_aborted_before_start");
    co_return unexpected(client_errc::operation_aborted);
  }

  // Defensive: ensure parser state is clean at the start of a handshake.
  // This prevents accidental carry-over between retries or reconnect attempts.
  parser_.reset();

  // Resolve:
  // - iocoro resolver runs getaddrinfo on a background thread_pool and resumes on this coroutine's
  //   executor (our strand).
  // - Cancellation is best-effort via stop_token. It cannot interrupt an in-flight getaddrinfo()
  //   but can prevent delivering results to the awaiting coroutine.
  iocoro::ip::tcp::resolver resolver{};
  auto resolve_res = resolver.async_resolve(cfg_.host, std::to_string(cfg_.port));
  if (cfg_.resolve_timeout.has_value()) {
    resolve_res = iocoro::with_timeout(std::move(resolve_res), *cfg_.resolve_timeout);
  }
  auto res = co_await std::move(resolve_res);
  if (!res) {
    REDISCORO_LOG_WARNING("resolve_failed err_code={}", res.error().value());
    if (res.error() == iocoro::error::timed_out) {
      co_return unexpected(client_errc::resolve_timeout);
    } else if (res.error() == iocoro::error::operation_aborted) {
      co_return unexpected(client_errc::operation_aborted);
    } else {
      co_return unexpected(client_errc::resolve_failed);
    }
  }
  if (res->empty()) {
    REDISCORO_LOG_WARNING("resolve_failed empty_endpoint_list");
    co_return unexpected(client_errc::resolve_failed);
  }

  if (tok.stop_requested()) {
    co_return unexpected(client_errc::operation_aborted);
  }

  // TCP connect with timeout (iterate endpoints in order).
  std::error_code connect_ec{};
  for (auto const& ep : *res) {
    // IMPORTANT: after a failed connect attempt, the socket may be left in a platform-dependent
    // error state. Always close before trying the next endpoint.
    if (socket_.is_open()) {
      (void)socket_.close();
    }

    auto connect_op = socket_.async_connect(ep);
    if (cfg_.connect_timeout.has_value()) {
      connect_op = iocoro::with_timeout(std::move(connect_op), *cfg_.connect_timeout);
    }
    auto connect_res = co_await std::move(connect_op);
    if (connect_res) {
      connect_ec = {};
      break;
    }
    connect_ec = connect_res.error();
  }

  if (connect_ec) {
    REDISCORO_LOG_WARNING("tcp_connect_failed err_code={}", connect_ec.value());
    // Map timeout/cancel vs generic connect failure.
    if (connect_ec == iocoro::error::timed_out) {
      co_return unexpected(client_errc::connect_timeout);
    } else if (connect_ec == iocoro::error::operation_aborted) {
      co_return unexpected(client_errc::operation_aborted);
    } else {
      error_info out{client_errc::connect_failed, connect_ec.message()};
      co_return unexpected(out);
    }
  }

  if (tok.stop_requested()) {
    co_return unexpected(client_errc::operation_aborted);
  }

  // Build handshake request (pipeline of commands).
  request req{};
  req.push("HELLO", "3");
  if (!cfg_.password.empty()) {
    if (!cfg_.username.empty()) {
      req.push("AUTH", cfg_.username, cfg_.password);
    } else {
      req.push("AUTH", cfg_.password);
    }
  }
  if (cfg_.database != 0) {
    req.push("SELECT", cfg_.database);
  }
  if (!cfg_.client_name.empty()) {
    req.push("CLIENT", "SETNAME", cfg_.client_name);
  }

  auto slot = std::make_shared<pending_dynamic_response<ignore_t>>(req.reply_count());

  if (!pipeline_.push(std::move(req), slot)) {
    REDISCORO_LOG_WARNING("handshake_enqueue_failed err_code={}",
                          make_error_code(client_errc::queue_full).value());
    co_return unexpected(client_errc::queue_full);
  }

  // Drive handshake IO directly (read/write loops are gated on OPEN so they will not interfere).
  auto do_handshake = [&]() -> iocoro::awaitable<iocoro::result<void>> {
    // Phase-1: flush the full handshake request first.
    // Handshake generates no additional writes after the initial request is fully sent.
    while (!tok.stop_requested() && pipeline_.has_pending_write()) {
      auto view = pipeline_.next_write_buffer();
      auto buf = std::as_bytes(std::span{view.data(), view.size()});
      auto w = co_await socket_.async_write_some(buf);
      if (!w) {
        if (w.error() == iocoro::error::operation_aborted) {
          co_return unexpected(client_errc::operation_aborted);
        }
        co_return unexpected(client_errc::handshake_failed);
      }
      pipeline_.on_write_done(*w);
    }

    // Phase-2: read/parse until the handshake sink completes.
    while (!tok.stop_requested() && !slot->is_complete()) {
      auto writable = parser_.prepare();
      auto r = co_await socket_.async_read_some(writable);
      if (!r) {
        if (r.error() == iocoro::error::operation_aborted) {
          co_return unexpected(client_errc::operation_aborted);
        }
        co_return unexpected(client_errc::handshake_failed);
      }
      if (*r == 0) {
        co_return unexpected(client_errc::connection_reset);
      }
      parser_.commit(*r);

      for (;;) {
        auto parsed = parser_.parse_one();
        if (!parsed) {
          if (pipeline_.has_pending_read()) {
            pipeline_.on_error(parsed.error());
          }
          co_return unexpected(parsed.error());
        }
        if (!parsed->has_value()) {
          break;
        }

        if (!pipeline_.has_pending_read()) {
          co_return unexpected(client_errc::unsolicited_message);
        }

        auto const root = **parsed;
        auto msg = resp3::build_message(parser_.tree(), root);
        pipeline_.on_message(std::move(msg));
        parser_.reclaim();

        if (slot->is_complete()) {
          break;
        }
      }
    }

    if (tok.stop_requested()) {
      co_return unexpected(client_errc::operation_aborted);
    }

    co_return iocoro::ok();
  };

  // Handshake timeout: prefer request_timeout if set, otherwise use connect_timeout.
  // If both are nullopt, no timeout is applied.
  iocoro::result<void> handshake_res;
  if (cfg_.request_timeout.has_value()) {
    handshake_res = co_await iocoro::with_timeout(do_handshake(), *cfg_.request_timeout);
  } else if (cfg_.connect_timeout.has_value()) {
    handshake_res = co_await iocoro::with_timeout(do_handshake(), *cfg_.connect_timeout);
  } else {
    handshake_res = co_await do_handshake();
  }

  if (!handshake_res) {
    REDISCORO_LOG_WARNING("handshake_io_failed err_code={}", handshake_res.error().value());
    auto fail_handshake = [&](error_info e) -> expected<void, error_info> {
      pipeline_.clear_all(e);
      return unexpected(std::move(e));
    };

    auto const ec = handshake_res.error();
    if (ec == iocoro::error::timed_out) {
      co_return fail_handshake(client_errc::handshake_timeout);
    }
    if (ec == iocoro::error::operation_aborted) {
      co_return fail_handshake(client_errc::operation_aborted);
    }
    if (is_protocol_error(ec)) {
      co_return fail_handshake(static_cast<protocol_errc>(ec.value()));
    }
    if (is_client_error(ec)) {
      co_return fail_handshake(static_cast<client_errc>(ec.value()));
    }

    co_return fail_handshake({client_errc::handshake_failed, ec.message()});
  }

  // Validate all handshake replies: any error => handshake_failed.
  //
  // Defensive: handshake_res == ok() implies slot should be complete (loop condition). Keep this
  // check to avoid future hangs if the handshake loop logic changes.
  if (!slot->is_complete()) {
    REDISCORO_LOG_WARNING("handshake_failed slot_incomplete");
    error_info e{client_errc::handshake_failed, "handshake slot incomplete"};
    pipeline_.clear_all(e);
    co_return unexpected(e);
  }
  auto results = co_await slot->wait();
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (!results[i]) {
      auto const& err = results[i].error();

      // If server error (AUTH/SELECT failed), preserve the detailed error.
      if (err.code.category() == server_category()) {
        REDISCORO_LOG_WARNING("handshake_reply_error err_code={}", err.code.value());
        co_return unexpected(err);
      }

      // For other errors, use handshake_failed but include the original error detail.
      error_info out{client_errc::handshake_failed, err.to_string()};
      REDISCORO_LOG_WARNING("handshake_reply_error err_code={}", out.code.value());
      pipeline_.clear_all(out);
      co_return unexpected(out);
    }
  }

  // Handshake succeeded.
  auto const from = state_;
  REDISCORO_LOG_INFO("state_transition from={} to={}", static_cast<int>(from),
                     static_cast<int>(connection_state::OPEN));
  set_state(connection_state::OPEN);
  reconnect_count_ = 0;
  generation_ += 1;
  emit_connection_event(connection_event{
    .kind = connection_event_kind::connected,
    .stage = connection_event_stage::handshake,
    .from_state = static_cast<std::int32_t>(from),
    .to_state = static_cast<std::int32_t>(connection_state::OPEN),
  });

  // Defensive: ensure parser buffer/state is clean when handing over to runtime loops.
  parser_.reset();
  co_return expected<void, error_info>{};
}

}  // namespace rediscoro::detail
