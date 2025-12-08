#pragma once

#include <xz/redis/adapter/detail/impl.hpp>
#include <xz/redis/adapter/detail/result_traits.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/node.hpp>
#include <xz/redis/response.hpp>

#include <limits>
#include <string_view>
#include <tuple>
#include <variant>

namespace xz::redis::adapter::detail {

template <template <class> class Adapter, typename Tuple>
struct transform_tuple;

template <template <class> class Adapter, typename... Ts>
struct transform_tuple<Adapter, std::tuple<Ts...>> {
  using type = std::tuple<Adapter<Ts>...>;
};

template <typename Tuple>
struct tuple_to_variant;

template <typename... Ts>
struct tuple_to_variant<std::tuple<Ts...>> {
  using type = std::variant<Ts...>;
};

template <class T>
auto internal_adapt(T& t) noexcept {
  return result_traits<std::decay_t<T>>::adapt(t);
}

template <std::size_t N>
struct assigner {
  template <class T1, class T2>
  static void assign(T1& dest, T2& from) {
    dest[N].template emplace<N>(internal_adapt(std::get<N>(from)));
    assigner<N - 1>::assign(dest, from);
  }
};

template <>
struct assigner<0> {
  template <class T1, class T2>
  static void assign(T1& dest, T2& from) {
    dest[0].template emplace<0>(internal_adapt(std::get<0>(from)));
  }
};

template <class R>
class static_adapter {
 private:
  static constexpr auto size = std::tuple_size<R>::value;
  using adapter_tuple = transform_tuple<adapter_t, R>::type;
  using variant_type = tuple_to_variant<adapter_tuple>::type;
  using adapters_array_type = std::array<variant_type, size>;

  adapters_array_type adapters_;
  std::size_t i_ = 0;

 public:
  explicit static_adapter(R& r) { assigner<size - 1>::assign(adapters_, r); }

  // clang-format off
  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    REDISUS_ASSERT(i_ < adapters_.size());
    std::visit(
      [&](auto& arg) {
         arg.on_msg(msg, ec);
      },
      adapters_.at(i_)
    );
    i_++;
  }
  // clang-format on
};

template <class>
struct response_traits;

template <>
struct response_traits<result<ignore_t>> {
  using response_type = result<ignore_t>;
  using adapter_type = ignore;

  static auto adapt(response_type&) noexcept { return ignore{}; }
};

template <class String, class Allocator>
struct response_traits<result<std::vector<resp3::basic_node<String>, Allocator>>> {
  using response_type = result<std::vector<resp3::basic_node<String>, Allocator>>;
  using adapter_type = general_aggregate<response_type>;

  static auto adapt(response_type& v) noexcept { return adapter_type{&v}; }
};

template <class... Ts>
struct response_traits<response<Ts...>> {
  using response_type = response<Ts...>;
  using adapter_type = static_adapter<response_type>;

  static auto adapt(response_type& r) noexcept { return adapter_type{r}; }
};

}  // namespace xz::redis::adapter::detail
