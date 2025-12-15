#pragma once

#include <xz/redis/resp3/node.hpp>
#include <xz/redis/adapter/detail/response_traits.hpp>

#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>

namespace xz::redis::adapter {

class any_adapter {
 public:
  using impl_t = std::function<void(resp3::msg_view const&)>;

  template <class T>
  static auto create_impl(T& resp) -> impl_t {
    return [adapter = detail::response_traits<T>::adapt(resp)](
               resp3::msg_view const& msg) mutable { adapter.on_msg(msg); };
  }

  any_adapter(impl_t fn = [](resp3::msg_view const&) {}) : impl_{std::move(fn)} {}

  template <class T, class = std::enable_if_t<!std::is_same_v<T, any_adapter>>>
  explicit any_adapter(T& resp) : impl_(create_impl(resp)) {}

  void on_msg(resp3::msg_view const& msg) { impl_(msg); };

 private:
  impl_t impl_;
};

}  // namespace xz::redis::adapter
