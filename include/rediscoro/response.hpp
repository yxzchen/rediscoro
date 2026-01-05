#pragma once

#include <rediscoro/adapter/adapt.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rediscoro {

struct redis_error {
  std::string message;
};

/// Wrapper around the internal response error variant.
/// Provides user-friendly inspection APIs without exposing std::variant in the surface.
class response_error {
public:
  using variant_type = std::variant<redis_error, resp3::error, adapter::error>;

  response_error(redis_error e) : v_(std::move(e)) {}
  response_error(resp3::error e) : v_(e) {}
  response_error(adapter::error e) : v_(std::move(e)) {}

  [[nodiscard]] bool is_redis_error() const noexcept { return std::holds_alternative<redis_error>(v_); }
  [[nodiscard]] bool is_resp3_error() const noexcept { return std::holds_alternative<resp3::error>(v_); }
  [[nodiscard]] bool is_adapter_error() const noexcept { return std::holds_alternative<adapter::error>(v_); }

  [[nodiscard]] const redis_error& as_redis_error() const { return std::get<redis_error>(v_); }
  [[nodiscard]] resp3::error as_resp3_error() const { return std::get<resp3::error>(v_); }
  [[nodiscard]] const adapter::error& as_adapter_error() const { return std::get<adapter::error>(v_); }

  [[nodiscard]] const variant_type& raw() const noexcept { return v_; }

private:
  variant_type v_;
};

class response_item {
public:
  using value_type = resp3::message;

  explicit response_item(resp3::message msg) : data_(std::move(msg)) {}
  explicit response_item(response_error err) : data_(std::move(err)) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<resp3::message>(data_); }

  [[nodiscard]] bool is_redis_error() const noexcept {
    if (!ok()) {
      return std::get<response_error>(data_).is_redis_error();
    }
    return false;
  }

  [[nodiscard]] bool is_resp3_error() const noexcept {
    if (!ok()) {
      return std::get<response_error>(data_).is_resp3_error();
    }
    return false;
  }

  [[nodiscard]] bool is_adapter_error() const noexcept {
    if (!ok()) {
      return std::get<response_error>(data_).is_adapter_error();
    }
    return false;
  }

  [[nodiscard]] const resp3::message& message() const { return std::get<resp3::message>(data_); }
  [[nodiscard]] const response_error& error() const { return std::get<response_error>(data_); }

  /// Convert a successful message into T via adapter::adapt<T>.
  /// If this item is already a failure, returns the existing error.
  template <typename T>
  [[nodiscard]] auto adapt_as() const -> rediscoro::expected<T, response_error> {
    if (!ok()) {
      return rediscoro::unexpected(error());
    }
    auto r = rediscoro::adapter::adapt<T>(message());
    if (!r) {
      return rediscoro::unexpected(response_error{std::move(r.error())});
    }
    return std::move(*r);
  }

  /// Helper: map RESP3 error replies (- / !) into redis_error; other values are ok().
  static auto from_message(resp3::message msg) -> response_item {
    if (msg.is<resp3::simple_error>()) {
      return response_item{response_error{redis_error{msg.as<resp3::simple_error>().message}}};
    }
    if (msg.is<resp3::bulk_error>()) {
      return response_item{response_error{redis_error{msg.as<resp3::bulk_error>().message}}};
    }
    return response_item{std::move(msg)};
  }

private:
  std::variant<resp3::message, response_error> data_;
};

class response {
public:
  response() = default;

  explicit response(std::size_t reserve_n) {
    items_.reserve(reserve_n);
  }

  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

  [[nodiscard]] const response_item& operator[](std::size_t i) const { return items_[i]; }
  [[nodiscard]] const response_item& at(std::size_t i) const { return items_.at(i); }

  [[nodiscard]] auto begin() const noexcept { return items_.begin(); }
  [[nodiscard]] auto end() const noexcept { return items_.end(); }

  void push(response_item item) {
    items_.push_back(std::move(item));
  }

  void push_message(resp3::message msg) {
    items_.push_back(response_item::from_message(std::move(msg)));
  }

  void push_redis_error(std::string message) {
    items_.push_back(response_item{response_error{redis_error{std::move(message)}}});
  }

  void push_resp3_error(resp3::error e) {
    items_.push_back(response_item{response_error{e}});
  }

  void push_adapter_error(adapter::error e) {
    items_.push_back(response_item{response_error{std::move(e)}});
  }

private:
  std::vector<response_item> items_{};
};

}  // namespace rediscoro


