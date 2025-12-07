#pragma once

#include <redisus/adapter/result.hpp>

#include <optional>

namespace redisus::adapter::detail {

template <class>
class wrapper;

template <class T>
class wrapper<result<T>> {
 public:
  using response_type = result<T>;

 private:
  response_type* result_;
  typename impl_map<T>::type impl_;

  bool set_if_resp3_error(resp3::msg_view const& msg) noexcept {
    auto const& node = msg.at(0);
    switch (node.data_type) {
      case resp3::type3::null:
      case resp3::type3::simple_error:
      case resp3::type3::blob_error:
        *result_ = unexpected(error{node.data_type, {std::cbegin(node.value()), std::cend(node.value())}});
        return true;
      default:
        return false;
    }
  }

 public:
  explicit wrapper(response_type* p) : result_(p) { result_->value() = T{}; }

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    REDISUS_ASSERT(!msg.empty());

    if (set_if_resp3_error(msg)) return;

    impl_.on_msg(result_->value(), msg, ec);
  }
};

template <class T>
class wrapper<result<std::optional<T>>> {
 public:
  using response_type = result<std::optional<T>>;

 private:
  response_type* result_;
  typename impl_map<T>::type impl_{};

  bool set_if_resp3_error(resp3::msg_view const& msg) noexcept {
    auto const& node = msg.at(0);
    switch (node.data_type) {
      case resp3::type3::blob_error:
      case resp3::type3::simple_error:
        *result_ = unexpected(error{node.data_type, {std::cbegin(node.value()), std::cend(node.value())}});
        return true;
      default:
        return false;
    }
  }

 public:
  explicit wrapper(response_type* p) : result_(p) {}

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    REDISUS_ASSERT(!msg.empty());

    if (set_if_resp3_error(msg)) return;

    if (msg.at(0).data_type == resp3::type3::null) return;

    result_->value() = T{};
    impl_.on_msg(result_->value().value(), msg, ec);
  }
};

}  // namespace redisus::adapter::detail
