#pragma once

#include <rediscoro/resp3/message.hpp>

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

/// Recursive visitor that traverses the entire message tree
template <typename Callback>
class recursive_visitor {
 public:
  Callback callback;

  explicit recursive_visitor(Callback cb) : callback(std::move(cb)) {}

  auto operator()(const simple_string& val) -> void { callback(val); }
  auto operator()(const simple_error& val) -> void { callback(val); }
  auto operator()(const integer& val) -> void { callback(val); }
  auto operator()(const double_type& val) -> void { callback(val); }
  auto operator()(const boolean& val) -> void { callback(val); }
  auto operator()(const big_number& val) -> void { callback(val); }
  auto operator()(const null& val) -> void { callback(val); }
  auto operator()(const bulk_string& val) -> void { callback(val); }
  auto operator()(const bulk_error& val) -> void { callback(val); }
  auto operator()(const verbatim_string& val) -> void { callback(val); }

  auto operator()(const array& val) -> void {
    callback(val);
    visit_elements(val.elements);
  }

  auto operator()(const map& val) -> void {
    callback(val);
    visit_entries(val.entries);
  }

  auto operator()(const set& val) -> void {
    callback(val);
    visit_elements(val.elements);
  }

  auto operator()(const attribute& val) -> void {
    callback(val);
    visit_entries(val.entries);
  }

  auto operator()(const push& val) -> void {
    callback(val);
    visit_elements(val.elements);
  }

  // Helper: visit a single message and its attributes
  auto visit_message(const message& msg) -> void {
    visit(*this, msg);
    if (msg.has_attributes()) {
      (*this)(msg.get_attributes());
    }
  }

private:
  // Helper: visit a collection of messages
  auto visit_elements(const auto& elements) -> void {
    for (const auto& elem : elements) {
      visit_message(elem);
    }
  }

  // Helper: visit map/attribute entries
  auto visit_entries(const auto& entries) -> void {
    for (const auto& [key, value] : entries) {
      visit_message(key);
      visit_message(value);
    }
  }
};

/// Helper function to create an optimized recursive visitor
template <typename Callback>
auto make_recursive_visitor(Callback&& cb) {
  return recursive_visitor<std::decay_t<Callback>>{std::forward<Callback>(cb)};
}

/// Walk through an entire message tree, calling the callback for each node
template <typename Callback>
auto walk(const message& msg, Callback&& callback) -> void {
  auto visitor = make_recursive_visitor(std::forward<Callback>(callback));
  visitor.visit_message(msg);
}

}  // namespace rediscoro::resp3
