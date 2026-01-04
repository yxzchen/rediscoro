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

/// Generic visitor base class with overloaded operator()
/// Users can inherit from this and override specific handlers
struct generic_visitor {
  // Simple types
  virtual auto operator()(const simple_string& val) -> void { on_simple_string(val); }
  virtual auto operator()(const simple_error& val) -> void { on_simple_error(val); }
  virtual auto operator()(const integer& val) -> void { on_integer(val); }
  virtual auto operator()(const double_type& val) -> void { on_double(val); }
  virtual auto operator()(const boolean& val) -> void { on_boolean(val); }
  virtual auto operator()(const big_number& val) -> void { on_big_number(val); }
  virtual auto operator()(const null& val) -> void { on_null(val); }

  // Bulk types
  virtual auto operator()(const bulk_string& val) -> void { on_bulk_string(val); }
  virtual auto operator()(const bulk_error& val) -> void { on_bulk_error(val); }
  virtual auto operator()(const verbatim_string& val) -> void { on_verbatim_string(val); }

  // Aggregate types
  virtual auto operator()(const array& val) -> void { on_array(val); }
  virtual auto operator()(const map& val) -> void { on_map(val); }
  virtual auto operator()(const set& val) -> void { on_set(val); }
  virtual auto operator()(const attribute& val) -> void { on_attribute(val); }
  virtual auto operator()(const push& val) -> void { on_push(val); }

protected:
  virtual ~generic_visitor() = default;

  virtual auto on_simple_string(const simple_string&) -> void {}
  virtual auto on_simple_error(const simple_error&) -> void {}
  virtual auto on_integer(const integer&) -> void {}
  virtual auto on_double(const double_type&) -> void {}
  virtual auto on_boolean(const boolean&) -> void {}
  virtual auto on_big_number(const big_number&) -> void {}
  virtual auto on_null(const null&) -> void {}
  virtual auto on_bulk_string(const bulk_string&) -> void {}
  virtual auto on_bulk_error(const bulk_error&) -> void {}
  virtual auto on_verbatim_string(const verbatim_string&) -> void {}
  virtual auto on_array(const array&) -> void {}
  virtual auto on_map(const map&) -> void {}
  virtual auto on_set(const set&) -> void {}
  virtual auto on_attribute(const attribute&) -> void {}
  virtual auto on_push(const push&) -> void {}
};

/// Recursive visitor that traverses the entire message tree
template <typename Callback>
struct recursive_visitor {
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
    for (const auto& elem : val.elements) {
      visit(*this, elem);
      if (elem.has_attributes()) {
        (*this)(elem.get_attributes());
      }
    }
  }

  auto operator()(const map& val) -> void {
    callback(val);
    for (const auto& [key, value] : val.entries) {
      visit(*this, key);
      if (key.has_attributes()) {
        (*this)(key.get_attributes());
      }
      visit(*this, value);
      if (value.has_attributes()) {
        (*this)(value.get_attributes());
      }
    }
  }

  auto operator()(const set& val) -> void {
    callback(val);
    for (const auto& elem : val.elements) {
      visit(*this, elem);
      if (elem.has_attributes()) {
        (*this)(elem.get_attributes());
      }
    }
  }

  auto operator()(const attribute& val) -> void {
    callback(val);
    for (const auto& [key, value] : val.entries) {
      visit(*this, key);
      if (key.has_attributes()) {
        (*this)(key.get_attributes());
      }
      visit(*this, value);
      if (value.has_attributes()) {
        (*this)(value.get_attributes());
      }
    }
  }

  auto operator()(const push& val) -> void {
    callback(val);
    for (const auto& elem : val.elements) {
      visit(*this, elem);
      if (elem.has_attributes()) {
        (*this)(elem.get_attributes());
      }
    }
  }
};

/// Helper function to create a recursive visitor
template <typename Callback>
auto make_recursive_visitor(Callback&& cb) {
  return recursive_visitor<std::decay_t<Callback>>{std::forward<Callback>(cb)};
}

/// Walk through an entire message tree, calling the callback for each node
template <typename Callback>
auto walk(const message& msg, Callback&& callback) -> void {
  auto visitor = make_recursive_visitor(std::forward<Callback>(callback));
  visit(visitor, msg);
  if (msg.has_attributes()) {
    visitor(msg.get_attributes());
  }
}

}  // namespace rediscoro::resp3
