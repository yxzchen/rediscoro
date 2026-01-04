#pragma once

#include <rediscoro/adapter/detail/impl.hpp>
#include <rediscoro/adapter/detail/result_traits.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/node.hpp>
#include <rediscoro/response.hpp>

#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace rediscoro::adapter::detail {

template <class T>
auto internal_adapt(T& t) noexcept {
  return result_traits<std::decay_t<T>>::adapt(t);
}

template <class R>
class static_adapter {
 private:
  static constexpr auto size = std::tuple_size<R>::value;
  using adapters_tuple_type =
      decltype([]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::tuple{adapter_t<std::tuple_element_t<Is, R>>{}...};
      }(std::make_index_sequence<size>{}));

  adapters_tuple_type adapters_;
  std::size_t i_ = 0;

 public:
  explicit static_adapter(R& r)
      : adapters_([]<std::size_t... Is>(R& rr, std::index_sequence<Is...>) {
          return std::tuple{internal_adapt(std::get<Is>(rr))...};
        }(r, std::make_index_sequence<size>{})) {}

  void on_msg(resp3::msg_view const& msg) {
    REDISCORO_ASSERT(i_ < size);

    []<std::size_t... Is>(std::size_t idx, adapters_tuple_type& adapters, resp3::msg_view const& m,
                          std::index_sequence<Is...>) {
      (void)(((idx == Is) ? (std::get<Is>(adapters).on_msg(m), true) : false) || ...);
    }(i_, adapters_, msg, std::make_index_sequence<size>{});

    ++i_;
  }
};

template <class R>
class vector_adapter {
 private:
  using adapter_type = adapter_t<R>;
  adapter_type adapter_;

 public:
  explicit vector_adapter(R& r) : adapter_(internal_adapt(r)) {}

  void on_msg(resp3::msg_view const& msg) { adapter_.on_msg(msg); }
};

template <class>
struct response_traits;

// template <>
// struct response_traits<result<ignore_t>> {
//   using response_type = result<ignore_t>;
//   using adapter_type = ignore;

//   static auto adapt(response_type& r) noexcept { return adapter_type{&r}; }
// };

template <class Allocator>
struct response_traits<result<std::vector<resp3::msg, Allocator>>> {
  using response_type = result<std::vector<resp3::msg, Allocator>>;
  using adapter_type = general_messages<response_type>;

  static auto adapt(response_type& v) noexcept { return adapter_type{&v}; }
};

template <class... Ts>
struct response_traits<response<Ts...>> {
  using response_type = response<Ts...>;
  using adapter_type = static_adapter<response_type>;

  static auto adapt(response_type& r) noexcept { return adapter_type{r}; }
};

template <class T>
struct response_traits<result<T>> {
  using response_type = result<T>;
  using adapter_type = result_traits<response_type>;

  static auto adapt(response_type& v) noexcept { return adapter_type::adapt(v); }
};

template <class T, class Allocator>
struct response_traits<std::vector<result<T>, Allocator>> {
  using response_type = std::vector<result<T>, Allocator>;
  using adapter_type = vector_adapter<response_type>;

  static auto adapt(response_type& v) noexcept { return adapter_type{v}; }
};

}  // namespace rediscoro::adapter::detail
