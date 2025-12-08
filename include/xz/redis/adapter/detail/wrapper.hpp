#pragma once

#include <xz/redis/adapter/result.hpp>
#include <xz/redis/resp3/node.hpp>

#include <optional>

namespace xz::redis::adapter::detail {

template <class T>
struct impl_map;

template <class Result>
inline bool set_error_from_resp3(Result& result, resp3::msg_view const& msg, bool null_is_error) noexcept {
  auto const& node = msg.at(0);
  if (is_error(node.data_type)) {
    result = unexpected(error{node.data_type, {std::cbegin(node.value()), std::cend(node.value())}});
    return true;
  }
  if (null_is_error && node.data_type == resp3::type3::null) {
    result = unexpected(error{node.data_type, {std::cbegin(node.value()), std::cend(node.value())}});
    return true;
  }
  return false;
}

template <class>
class wrapper;

template <class T>
class wrapper<result<T>> {
 public:
  using response_type = result<T>;

 private:
  response_type* result_;
  typename impl_map<T>::type impl_;

 public:
  explicit wrapper(response_type* p = nullptr) : result_(p) {}

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    REDISXZ_ASSERT(!msg.empty());
    if (set_error_from_resp3(*result_, msg, true)) return;
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

 public:
  explicit wrapper(response_type* p = nullptr) : result_(p) {}

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    REDISXZ_ASSERT(!msg.empty());
    if (set_error_from_resp3(*result_, msg, false)) return;
    if (msg.at(0).data_type == resp3::type3::null) return;
    result_->value().emplace();
    impl_.on_msg(result_->value().value(), msg, ec);
  }
};

}  // namespace xz::redis::adapter::detail
