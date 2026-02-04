#pragma once

#include <rediscoro/resp3/message.hpp>

#include <type_traits>
#include <utility>

namespace rediscoro::resp3 {

/// Visit a message with a visitor callable
/// The visitor should have overloaded operator() for each value type
template <typename Visitor>
auto visit(Visitor&& visitor, message& msg) -> decltype(auto) {
  return std::visit(std::forward<Visitor>(visitor), msg.value);
}

template <typename Visitor>
auto visit(Visitor&& visitor, const message& msg) -> decltype(auto) {
  return std::visit(std::forward<Visitor>(visitor), msg.value);
}

namespace detail {

template <typename Callback>
auto walk_message(const message& msg, Callback& callback) -> void;

template <typename Callback>
auto walk_attribute(const attribute& attr, Callback& callback) -> void {
  callback(attr);
  for (const auto& [k, v] : attr.entries) {
    walk_message(k, callback);
    walk_message(v, callback);
  }
}

template <typename Callback>
struct walker {
  Callback* cb{};

  auto operator()(const simple_string& v) -> void { (*cb)(v); }
  auto operator()(const simple_error& v) -> void { (*cb)(v); }
  auto operator()(const integer& v) -> void { (*cb)(v); }
  auto operator()(const double_number& v) -> void { (*cb)(v); }
  auto operator()(const boolean& v) -> void { (*cb)(v); }
  auto operator()(const big_number& v) -> void { (*cb)(v); }
  auto operator()(const null& v) -> void { (*cb)(v); }
  auto operator()(const bulk_string& v) -> void { (*cb)(v); }
  auto operator()(const bulk_error& v) -> void { (*cb)(v); }
  auto operator()(const verbatim_string& v) -> void { (*cb)(v); }

  auto operator()(const array& v) -> void {
    (*cb)(v);
    for (const auto& e : v.elements) {
      walk_message(e, *cb);
    }
  }

  auto operator()(const map& v) -> void {
    (*cb)(v);
    for (const auto& [k, val] : v.entries) {
      walk_message(k, *cb);
      walk_message(val, *cb);
    }
  }

  auto operator()(const set& v) -> void {
    (*cb)(v);
    for (const auto& e : v.elements) {
      walk_message(e, *cb);
    }
  }

  auto operator()(const push& v) -> void {
    (*cb)(v);
    for (const auto& e : v.elements) {
      walk_message(e, *cb);
    }
  }
};

template <typename Callback>
auto walk_message(const message& msg, Callback& callback) -> void {
  walker<Callback> w{.cb = &callback};
  visit(w, msg);
  if (msg.has_attributes()) {
    walk_attribute(msg.get_attributes(), callback);
  }
}

}  // namespace detail

/// Walk through an entire message tree, calling the callback for each node
template <typename Callback>
auto walk(const message& msg, Callback&& callback) -> void {
  std::decay_t<Callback> cb{std::forward<Callback>(callback)};
  detail::walk_message(msg, cb);
}

}  // namespace rediscoro::resp3
