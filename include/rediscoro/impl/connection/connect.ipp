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
  auto res = co_await iocoro::with_timeout(
    resolver.async_resolve(cfg_.host, std::to_string(cfg_.port)), cfg_.resolve_timeout);
  if (!res) {
    if (res.error() == iocoro::error::timed_out) {
      co_return unexpected(client_errc::resolve_timeout);
    } else if (res.error() == iocoro::error::operation_aborted) {
      co_return unexpected(client_errc::operation_aborted);
    } else {
      co_return unexpected(client_errc::resolve_failed);
    }
  }
  if (res->empty()) {
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

    auto connect_res =
      co_await iocoro::with_timeout(socket_.async_connect(ep), cfg_.connect_timeout);
    if (connect_res) {
      connect_ec = {};
      break;
    }
    connect_ec = connect_res.error();
  }

  if (connect_ec) {
    // Map timeout/cancel vs generic connect failure.
    if (connect_ec == iocoro::error::timed_out) {
      co_return unexpected(client_errc::connect_timeout);
    } else if (connect_ec == iocoro::error::operation_aborted) {
      co_return unexpected(client_errc::operation_aborted);
    } else {
      co_return unexpected(client_errc::connect_failed);
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

  if (cfg_.trace_handshake && cfg_.trace_hooks.enabled()) {
    auto const hooks = cfg_.trace_hooks;  // copy for stability
    const auto start = std::chrono::steady_clock::now();
    const request_trace_info info{
      .id = next_request_id_++,
      .kind = request_kind::handshake,
      .command_count = req.command_count(),
      .wire_bytes = req.wire().size(),
    };

    if (hooks.on_start != nullptr) {
      request_trace_start evt{.info = info};
      try {
        hooks.on_start(hooks.user_data, evt);
      } catch (...) {
      }
    }
    slot->set_trace_context(hooks, info, start);
  }

  pipeline_.push(std::move(req), slot);

  // Drive handshake IO directly (read/write loops are gated on OPEN so they will not interfere).
  auto handshake_res = co_await iocoro::with_timeout(
    [&]() -> iocoro::awaitable<iocoro::result<void>> {
      // Phase-1: flush the full handshake request first.
      // Handshake generates no additional writes after the initial request is fully sent.
      while (!tok.stop_requested() && pipeline_.has_pending_write()) {
        auto view = pipeline_.next_write_buffer();
        auto buf = std::as_bytes(std::span{view.data(), view.size()});
        auto w = co_await socket_.async_write_some(buf);
        if (!w) {
          if (w.error() == iocoro::error::operation_aborted) {
            co_return iocoro::unexpected(client_errc::operation_aborted);
          }
          co_return iocoro::unexpected(client_errc::handshake_failed);
        }
        pipeline_.on_write_done(*w);
      }

      // Phase-2: read/parse until the handshake sink completes.
      while (!tok.stop_requested() && !slot->is_complete()) {
        auto writable = parser_.prepare();
        auto r = co_await socket_.async_read_some(writable);
        if (!r) {
          if (r.error() == iocoro::error::operation_aborted) {
            co_return iocoro::unexpected(client_errc::operation_aborted);
          }
          co_return iocoro::unexpected(client_errc::handshake_failed);
        }
        if (*r == 0) {
          co_return iocoro::unexpected(client_errc::connection_reset);
        }
        parser_.commit(*r);

        for (;;) {
          auto parsed = parser_.parse_one();
          if (!parsed) {
            if (pipeline_.has_pending_read()) {
              pipeline_.on_error(parsed.error());
            }
            co_return iocoro::unexpected(parsed.error());
          }
          if (!parsed->has_value()) {
            break;
          }

          if (!pipeline_.has_pending_read()) {
            co_return iocoro::unexpected(client_errc::unsolicited_message);
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
        co_return iocoro::unexpected(client_errc::operation_aborted);
      }

      co_return iocoro::ok();
    }(),
    cfg_.connect_timeout);

  if (!handshake_res) {
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

    co_return fail_handshake(client_errc::handshake_failed);
  }

  // Validate all handshake replies: any error => handshake_failed.
  //
  // Defensive: handshake_res == ok() implies slot should be complete (loop condition). Keep this
  // check to avoid future hangs if the handshake loop logic changes.
  if (!slot->is_complete()) {
    auto e = client_errc::handshake_failed;
    pipeline_.clear_all(e);
    co_return unexpected(e);
  }
  auto results = co_await slot->wait();
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (!results[i]) {
      auto e = client_errc::handshake_failed;
      pipeline_.clear_all(e);
      co_return unexpected(e);
    }
  }

  // Handshake succeeded.
  state_ = connection_state::OPEN;
  reconnect_count_ = 0;

  // Defensive: ensure parser buffer/state is clean when handing over to runtime loops.
  parser_.reset();
  co_return expected<void, error_info>{};
}

}  // namespace rediscoro::detail
