#pragma once

#include <xz/redis/config.hpp>
#include <xz/redis/error.hpp>

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

// FSM Actions: Data-free signals (FSM decides "what", Connection decides "how")
namespace fsm_action {
struct state_change {
  connection_state old_state;
  connection_state new_state;
};
struct send_hello {};
struct send_auth {};
struct send_select {};
struct send_clientname {};
struct connection_ready {};
struct connection_failed {
  std::error_code ec;
};
}  // namespace fsm_action

using fsm_action_variant = std::variant<fsm_action::state_change, fsm_action::send_hello, fsm_action::send_auth,
                                        fsm_action::send_select, fsm_action::send_clientname,
                                        fsm_action::connection_ready, fsm_action::connection_failed>;

using fsm_output = std::vector<fsm_action_variant>;

/**
 * @brief Pure state machine for Redis connection handshake
 *
 * Design principles:
 * - FSM is synchronous and non-reentrant
 * - FSM knows only state transitions, not protocol details
 * - FSM outputs semantic actions (what to do), not serialized commands (how to do)
 * - Connection layer handles RESP semantics and command serialization
 *
 * Assumptions (RESP3 handshake):
 * - HELLO/AUTH/SELECT/CLIENT SETNAME are single-response commands
 * - Each command receives exactly one semantic result (ok/error)
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
    auto old_state = state_;
    state_ = connection_state::handshaking;
    return {fsm_action::state_change{old_state, state_}, fsm_action::send_hello{}};
  }

  /** Called when connection is explicitly closed */
  auto on_closed() -> fsm_output {
    auto old_state = state_;
    state_ = connection_state::disconnected;
    if (old_state == state_) {
      return {};
    }
    return {fsm_action::state_change{old_state, state_}};
  }

  // === Transport Errors (not command-specific) ===

  /** Socket error, timeout, EOF, protocol framing error */
  auto on_io_error(std::error_code ec) -> fsm_output {
    auto old_state = state_;
    state_ = connection_state::failed;
    fsm_output out;
    if (old_state != state_) {
      out.push_back(fsm_action::state_change{old_state, state_});
    }
    out.push_back(fsm_action::connection_failed{ec});
    return out;
  }

  // === Semantic Events (command responses) ===

  auto on_hello_ok() -> fsm_output {
    if (state_ != connection_state::handshaking) {
      return {};
    }
    return advance_after_hello();
  }

  auto on_hello_error(std::error_code ec) -> fsm_output {
    if (state_ != connection_state::handshaking) {
      return {};
    }
    return fail(ec);
  }

  auto on_auth_ok() -> fsm_output {
    if (state_ != connection_state::authenticating) {
      return {};
    }
    return advance_after_auth();
  }

  auto on_auth_error(std::error_code ec) -> fsm_output {
    if (state_ != connection_state::authenticating) {
      return {};
    }
    return fail(ec);
  }

  auto on_select_ok() -> fsm_output {
    if (state_ != connection_state::selecting_db) {
      return {};
    }
    return advance_after_select();
  }

  auto on_select_error(std::error_code ec) -> fsm_output {
    if (state_ != connection_state::selecting_db) {
      return {};
    }
    return fail(ec);
  }

  auto on_clientname_ok() -> fsm_output {
    if (state_ != connection_state::setting_clientname) {
      return {};
    }
    return advance_after_clientname();
  }

  auto on_clientname_error(std::error_code ec) -> fsm_output {
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
    if (needs_auth()) {
      auto old_state = state_;
      state_ = connection_state::authenticating;
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_auth{}};
    }
    return advance_after_auth();
  }

  auto advance_after_auth() -> fsm_output {
    if (cfg_.database != 0) {
      auto old_state = state_;
      state_ = connection_state::selecting_db;
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_select{}};
    }
    return advance_after_select();
  }

  auto advance_after_select() -> fsm_output {
    if (cfg_.client_name.has_value()) {
      auto old_state = state_;
      state_ = connection_state::setting_clientname;
      return {fsm_action::state_change{old_state, state_}, fsm_action::send_clientname{}};
    }
    return advance_after_clientname();
  }

  auto advance_after_clientname() -> fsm_output {
    auto old_state = state_;
    state_ = connection_state::ready;
    return {fsm_action::state_change{old_state, state_}, fsm_action::connection_ready{}};
  }

  auto needs_auth() const noexcept -> bool { return cfg_.password.has_value(); }

  config const& cfg_;
  connection_state state_;
};

}  // namespace xz::redis
