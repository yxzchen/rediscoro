#pragma once

#include <redisus/resp3/node.hpp>
#include <redisus/adapter/detail/response_traits.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>

namespace redisus::adapter {

class any_adapter {
 public:
  using impl_t = std::function<void(resp3::msg_view const&, std::error_code&)>;

  template <class T>
  static auto create_impl(T& resp) -> impl_t {
    return [adapter = detail::response_traits<T>::adapt(resp)](
               resp3::msg_view const& msg, std::error_code& ec) mutable { adapter.on_msg(msg, ec); };
  }

  any_adapter(impl_t fn = [](resp3::msg_view const&, std::error_code&) {}) : impl_{std::move(fn)} {}

  template <class T, class = std::enable_if_t<!std::is_same_v<T, any_adapter>>>
  explicit any_adapter(T& resp) : impl_(create_impl(resp)) {}

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) { impl_(msg, ec); };

 private:
  impl_t impl_;
};

}  // namespace redisus::adapter
