#pragma once

#include <xz/redis/config.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
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
struct send_data {
  std::string data;
};
struct state_changed {
  connection_state from;
  connection_state to;
};
struct connection_ready {};
struct connection_failed {
  std::error_code ec;
  std::string reason;
};
}  // namespace fsm_action

using fsm_action_variant = std::variant<fsm_action::send_data, fsm_action::state_changed, fsm_action::connection_ready,
                                        fsm_action::connection_failed>;

struct fsm_output {
  std::vector<fsm_action_variant> actions;
  void push(fsm_action_variant&& a) { actions.emplace_back(std::move(a)); }
};

class connection_fsm {
 public:
  explicit connection_fsm(config const& cfg) noexcept : cfg_(cfg), state_(connection_state::disconnected) {}

  auto current_state() const noexcept -> connection_state { return state_; }

  auto on_connected() -> fsm_output {
    fsm_output out;
    if (state_ != connection_state::disconnected) {
      return out;
    }

    change_state(connection_state::handshaking, out);
    out.push(fsm_action::send_data{make_hello_payload()});
    return out;
  }

  auto on_closed() -> fsm_output {
    fsm_output out;
    change_state(connection_state::disconnected, out);
    return out;
  }

  auto on_connection_failed(std::error_code ec) -> fsm_output {
    fsm_output out;
    change_state(connection_state::failed, out);
    out.push(fsm_action::connection_failed{ec, "underlying connection failed"});
    return out;
  }

  auto on_data_received(resp3::detail::generator<std::optional<std::vector<resp3::node_view>>>& gen) -> fsm_output {
    fsm_output out;

    while (true) {
      bool ok = false;
      try {
        ok = gen.next();
      } catch (std::exception const& e) {
        change_state(connection_state::failed, out);
        out.push(fsm_action::connection_failed{error::resp3_protocol, std::string("parser exception: ") + e.what()});
        return out;
      } catch (...) {
        change_state(connection_state::failed, out);
        out.push(fsm_action::connection_failed{error::resp3_protocol, "parser threw unknown exception"});
        return out;
      }

      if (!ok) {
        break;
      }

      auto msg_opt = gen.value();
      if (!msg_opt) {
        break;
      }

      auto& msg = *msg_opt;
      if (msg.empty()) {
        continue;
      }

      switch (state_) {
        case connection_state::handshaking:
          append_output(out, handle_hello_response(msg));
          break;
        case connection_state::authenticating:
          append_output(out, handle_auth_response(msg));
          break;
        case connection_state::selecting_db:
          append_output(out, handle_select_response(msg));
          break;
        case connection_state::setting_clientname:
          append_output(out, handle_clientname_response(msg));
          break;
        default:
          break;
      }

      if (state_ == connection_state::failed) {
        break;
      }
    }

    return out;
  }

  void reset() noexcept { state_ = connection_state::disconnected; }

 private:
  static void append_output(fsm_output& dst, fsm_output const& src) {
    for (auto& a : src.actions) {
      dst.actions.emplace_back(std::move(a));
    }
  }

  void change_state(connection_state s, fsm_output& out) {
    if (state_ == s) {
      return;
    }
    auto old = state_;
    state_ = s;
    out.push(fsm_action::state_changed{old, s});
  }

  static auto is_error(resp3::node_view const& n) noexcept -> bool {
    return n.data_type == resp3::type3::blob_error || n.data_type == resp3::type3::simple_error;
  }

  static auto make_hello_payload() -> std::string {
    request req;
    req.push("HELLO", "3");
    return std::string(req.payload());
  }

  static auto make_auth_payload(config const& cfg) -> std::string {
    request req;
    if (cfg.username) {
      req.push("AUTH", *cfg.username, *cfg.password);
    } else {
      req.push("AUTH", *cfg.password);
    }
    return std::string(req.payload());
  }

  static auto make_select_payload(int db) -> std::string {
    request req;
    req.push("SELECT", std::to_string(db));
    return std::string(req.payload());
  }

  static auto make_clientname_payload(std::string_view n) -> std::string {
    request req;
    req.push("CLIENT", "SETNAME", std::string(n));
    return std::string(req.payload());
  }

  auto fail(std::error_code ec, std::string_view reason) -> fsm_output {
    fsm_output out;
    change_state(connection_state::failed, out);
    out.push(fsm_action::connection_failed{ec, std::string(reason)});
    return out;
  }

  auto validate_response(std::vector<resp3::node_view> const& msg, error err_code, std::string_view operation)
      -> std::optional<fsm_output> {
    if (msg.empty()) {
      return fail(error::resp3_protocol, std::string("empty ") + std::string(operation));
    }
    if (is_error(msg[0])) {
      std::string reason = std::string(operation) + " failed";
      if (!msg[0].value().empty()) {
        reason += ": " + std::string(msg[0].value());
      }
      return fail(err_code, reason);
    }
    return std::nullopt;
  }

  auto complete_setup_after_hello() -> fsm_output {
    if (needs_auth()) {
      fsm_output out;
      change_state(connection_state::authenticating, out);
      out.push(fsm_action::send_data{make_auth_payload(cfg_)});
      return out;
    }
    return complete_setup_after_auth();
  }

  auto complete_setup_after_auth() -> fsm_output {
    if (cfg_.database != 0) {
      fsm_output out;
      change_state(connection_state::selecting_db, out);
      out.push(fsm_action::send_data{make_select_payload(cfg_.database)});
      return out;
    }
    return complete_setup_after_select();
  }

  auto complete_setup_after_select() -> fsm_output {
    if (cfg_.client_name) {
      fsm_output out;
      change_state(connection_state::setting_clientname, out);
      out.push(fsm_action::send_data{make_clientname_payload(*cfg_.client_name)});
      return out;
    }
    return complete_setup_after_setname();
  }

  auto complete_setup_after_setname() -> fsm_output {
    fsm_output out;
    change_state(connection_state::ready, out);
    out.push(fsm_action::connection_ready{});
    return out;
  }

  auto handle_hello_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (auto err = validate_response(msg, error::resp3_hello, "HELLO")) {
      return *err;
    }
    return complete_setup_after_hello();
  }

  auto handle_auth_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (auto err = validate_response(msg, error::auth_failed, "AUTH")) {
      return *err;
    }
    return complete_setup_after_auth();
  }

  auto handle_select_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (auto err = validate_response(msg, error::select_db_failed, "SELECT")) {
      return *err;
    }
    return complete_setup_after_select();
  }

  auto handle_clientname_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (auto err = validate_response(msg, error::client_setname_failed, "CLIENT SETNAME")) {
      return *err;
    }
    return complete_setup_after_setname();
  }

  auto needs_auth() const noexcept -> bool { return cfg_.password.has_value(); }

  config cfg_;
  connection_state state_;
};

}  // namespace xz::redis
