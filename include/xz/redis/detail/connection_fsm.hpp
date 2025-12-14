#pragma once

#include <xz/redis/config.hpp>
#include <xz/redis/detail/assert.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/request.hpp>

#include <cassert>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace xz::redis {

enum class connection_state {
  disconnected,
  handshaking,
  authenticating,
  selecting_db,
  setting_clientname,
  ready,
  failed,
};

namespace fsm_action {
struct state_change {
  connection_state old_state;
  connection_state new_state;
};
struct send_request {
  request req;
};
struct connection_ready {};
struct connection_failed {
  std::error_code ec;
};
}  // namespace fsm_action

using fsm_action_variant =
    std::variant<fsm_action::state_change, fsm_action::send_request, fsm_action::connection_ready,
                 fsm_action::connection_failed>;

using fsm_output = std::vector<fsm_action_variant>;

/**
 * @brief Pure state machine for Redis connection handshake
 *
 * Design principles:
 * - FSM is synchronous and non-reentrant
 * - FSM knows only state transitions, not protocol details
 * - FSM outputs complete requests with actual data
 * - Connection layer just executes the requests
 *
 * Assumptions (RESP3 handshake):
 * - HELLO/AUTH/SELECT/CLIENT SETNAME are single-response commands
 * - Each command receives exactly one semantic result (ok/error)
 *
 * Invariants:
 * - failed is a terminal state: only reset() can transition out
 * - All events except reset() are no-op in failed state
 */
class connection_fsm {
 public:
  explicit connection_fsm(config const& cfg) noexcept : cfg_(cfg), state_(connection_state::disconnected) {}

  auto current_state() const noexcept -> connection_state { return state_; }

  // === Lifecycle Events ===

  /** Called after TCP connection established */
  auto on_connected() -> fsm_output {
    if (state_ != connection_state::disconnected) {
      return {};
    }

    if (cfg_.needs_hello) {
      auto old_state = state_;
      state_ = connection_state::handshaking;
      request req;
      req.push("HELLO", 3);
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_request{std::move(req)}};
    }

    return advance_after_hello();
  }

  // === Transport Errors (not command-specific) ===

  /**
   * @brief Socket error, timeout, EOF, RST, or user-initiated close
   *
   * All connection failures go through this single error path.
   */
  auto on_io_error(std::error_code ec) -> fsm_output {
    if (state_ == connection_state::failed) {
      return {};
    }
    auto old_state = state_;
    state_ = connection_state::failed;
    fsm_output out;
    if (old_state != state_) {
      out.push_back(fsm_action::state_change{old_state, state_});
    }
    out.push_back(fsm_action::connection_failed{ec});
    return out;
  }

  auto on_hello_ok() -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::handshaking, "on_hello_ok() called in wrong state");

    if (state_ != connection_state::handshaking) {
      return {};
    }
    return advance_after_hello();
  }

  auto on_hello_error(std::error_code ec) -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::handshaking, "on_hello_error() called in wrong state");

    if (state_ != connection_state::handshaking) {
      return {};
    }
    return fail(ec);
  }

  auto on_auth_ok() -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::authenticating, "on_auth_ok() called in wrong state");

    if (state_ != connection_state::authenticating) {
      return {};
    }
    return advance_after_auth();
  }

  auto on_auth_error(std::error_code ec) -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::authenticating, "on_auth_error() called in wrong state");

    if (state_ != connection_state::authenticating) {
      return {};
    }
    return fail(ec);
  }

  auto on_select_ok() -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::selecting_db, "on_select_ok() called in wrong state");

    if (state_ != connection_state::selecting_db) {
      return {};
    }
    return advance_after_select();
  }

  auto on_select_error(std::error_code ec) -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::selecting_db, "on_select_error() called in wrong state");

    if (state_ != connection_state::selecting_db) {
      return {};
    }
    return fail(ec);
  }

  auto on_clientname_ok() -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::setting_clientname, "on_clientname_ok() called in wrong state");

    if (state_ != connection_state::setting_clientname) {
      return {};
    }
    return advance_after_clientname();
  }

  auto on_clientname_error(std::error_code ec) -> fsm_output {
    REDISXZ_ASSERT(state_ == connection_state::setting_clientname, "on_clientname_error() called in wrong state");

    if (state_ != connection_state::setting_clientname) {
      return {};
    }
    return fail(ec);
  }

  void reset() noexcept { state_ = connection_state::disconnected; }

 private:
  auto fail(std::error_code ec) -> fsm_output {
    auto old_state = state_;
    state_ = connection_state::failed;
    fsm_output out;
    if (old_state != state_) {
      out.push_back(fsm_action::state_change{old_state, state_});
    }
    out.push_back(fsm_action::connection_failed{ec});
    return out;
  }

  auto advance_after_hello() -> fsm_output {
    if (cfg_.password.has_value()) {
      auto old_state = state_;
      state_ = connection_state::authenticating;
      // AUTH with actual password
      request req;
      if (cfg_.username.has_value()) {
        req.push("AUTH", *cfg_.username, *cfg_.password);
      } else {
        req.push("AUTH", *cfg_.password);
      }
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_request{std::move(req)}};
    }
    return advance_after_auth();
  }

  auto advance_after_auth() -> fsm_output {
    if (cfg_.database != 0) {
      auto old_state = state_;
      state_ = connection_state::selecting_db;
      // SELECT with actual database
      request req;
      req.push("SELECT", cfg_.database);
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_request{std::move(req)}};
    }
    return advance_after_select();
  }

  auto advance_after_select() -> fsm_output {
    if (cfg_.client_name.has_value()) {
      auto old_state = state_;
      state_ = connection_state::setting_clientname;
      // CLIENT SETNAME with actual name
      request req;
      req.push("CLIENT", "SETNAME", *cfg_.client_name);
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_request{std::move(req)}};
    }
    return advance_after_clientname();
  }

  auto advance_after_clientname() -> fsm_output {
    auto old_state = state_;
    state_ = connection_state::ready;
    return {fsm_action::state_change{old_state, state_}, fsm_action::connection_ready{}};
  }

  config const& cfg_;
  connection_state state_;
};

}  // namespace xz::redis
