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
      } catch (...) {
        change_state(connection_state::failed, out);
        out.push(fsm_action::connection_failed{make_error_code(error::resp3_protocol), "parser threw exception"});
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
    for (auto const& a : src.actions) {
      dst.actions.push_back(a);
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

  auto handle_hello_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (msg.empty()) {
      return fail(make_error_code(error::resp3_protocol), "empty HELLO");
    }

    if (is_error(msg[0])) {
      return fail(make_error_code(error::resp3_hello), "HELLO returned error");
    }

    if (needs_auth()) {
      fsm_output out;
      change_state(connection_state::authenticating, out);
      out.push(fsm_action::send_data{make_auth_payload(cfg_)});
      return out;
    }

    if (cfg_.database != 0) {
      fsm_output out;
      change_state(connection_state::selecting_db, out);
      out.push(fsm_action::send_data{make_select_payload(cfg_.database)});
      return out;
    }

    if (cfg_.client_name) {
      fsm_output out;
      change_state(connection_state::setting_clientname, out);
      out.push(fsm_action::send_data{make_clientname_payload(*cfg_.client_name)});
      return out;
    }

    fsm_output out;
    change_state(connection_state::ready, out);
    out.push(fsm_action::connection_ready{});
    return out;
  }

  auto handle_auth_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (msg.empty()) {
      return fail(make_error_code(error::resp3_protocol), "empty AUTH");
    }

    if (is_error(msg[0])) {
      return fail(make_error_code(error::auth_failed), "AUTH failed");
    }

    if (cfg_.database != 0) {
      fsm_output out;
      change_state(connection_state::selecting_db, out);
      out.push(fsm_action::send_data{make_select_payload(cfg_.database)});
      return out;
    }

    if (cfg_.client_name) {
      fsm_output out;
      change_state(connection_state::setting_clientname, out);
      out.push(fsm_action::send_data{make_clientname_payload(*cfg_.client_name)});
      return out;
    }

    fsm_output out;
    change_state(connection_state::ready, out);
    out.push(fsm_action::connection_ready{});
    return out;
  }

  auto handle_select_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (msg.empty()) {
      return fail(make_error_code(error::resp3_protocol), "empty SELECT");
    }
    if (is_error(msg[0])) {
      return fail(make_error_code(error::select_db_failed), "SELECT failed");
    }

    if (cfg_.client_name) {
      fsm_output out;
      change_state(connection_state::setting_clientname, out);
      out.push(fsm_action::send_data{make_clientname_payload(*cfg_.client_name)});
      return out;
    }

    fsm_output out;
    change_state(connection_state::ready, out);
    out.push(fsm_action::connection_ready{});
    return out;
  }

  auto handle_clientname_response(std::vector<resp3::node_view> const& msg) -> fsm_output {
    if (msg.empty()) {
      return fail(make_error_code(error::resp3_protocol), "empty CLIENT SETNAME");
    }
    if (is_error(msg[0])) {
      return fail(make_error_code(error::client_setname_failed), "CLIENT SETNAME failed");
    }

    fsm_output out;
    change_state(connection_state::ready, out);
    out.push(fsm_action::connection_ready{});
    return out;
  }

  auto needs_auth() const noexcept -> bool { return cfg_.password.has_value(); }

  config cfg_;
  connection_state state_;
};

}  // namespace xz::redis
