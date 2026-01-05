#pragma once

#include <rediscoro/adapter/adapt.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/assert.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

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

template <typename T>
using response_slot = expected<T, response_error>;

namespace detail {

template <typename T>
using slot_storage = std::optional<response_slot<T>>;

template <std::size_t I, typename... Ts>
using nth_type = std::tuple_element_t<I, std::tuple<Ts...>>;

}  // namespace detail

/// Typed response for a pipeline (compile-time sized, heterogenous slots).
template <typename... Ts>
class response {
public:
  static constexpr std::size_t static_size = sizeof...(Ts);

  response() = default;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return static_size; }
  [[nodiscard]] static constexpr bool empty() noexcept { return static_size == 0; }

  template <std::size_t I>
  [[nodiscard]] auto get() -> response_slot<detail::nth_type<I, Ts...>>& {
    auto& opt = std::get<I>(results_);
    REDISCORO_ASSERT(opt.has_value());
    return *opt;
  }

  template <std::size_t I>
  [[nodiscard]] auto get() const -> const response_slot<detail::nth_type<I, Ts...>>& {
    const auto& opt = std::get<I>(results_);
    REDISCORO_ASSERT(opt.has_value());
    return *opt;
  }

  /// For exec: set I-th slot from a successfully parsed RESP3 message.
  template <std::size_t I>
  void set_from_message(resp3::message msg) {
    using T = detail::nth_type<I, Ts...>;

    if (msg.is<resp3::simple_error>()) {
      set_error<I>(redis_error{msg.as<resp3::simple_error>().message});
      return;
    }
    if (msg.is<resp3::bulk_error>()) {
      set_error<I>(redis_error{msg.as<resp3::bulk_error>().message});
      return;
    }

    auto r = adapter::adapt<T>(msg);
    if (!r) {
      set_error<I>(std::move(r.error()));
      return;
    }

    std::get<I>(results_) = std::move(*r);
  }

  /// For exec: set I-th slot from a protocol/parse error.
  template <std::size_t I>
  void set_resp3_error(resp3::error e) {
    set_error<I>(e);
  }

  /// For exec: set I-th slot from a redis error (already extracted).
  template <std::size_t I>
  void set_redis_error(std::string message) {
    set_error<I>(redis_error{std::move(message)});
  }

  template <std::size_t I>
  void set_adapter_error(adapter::error e) {
    set_error<I>(std::move(e));
  }

  /// Convenience: unpack as tuple of references for structured bindings.
  [[nodiscard]] auto unpack() -> std::tuple<response_slot<Ts>&...> {
    return unpack_impl(std::index_sequence_for<Ts...>{});
  }

  [[nodiscard]] auto unpack() const -> std::tuple<const response_slot<Ts>&...> {
    return unpack_impl(std::index_sequence_for<Ts...>{});
  }

private:
  std::tuple<detail::slot_storage<Ts>...> results_{};

  template <std::size_t... Is>
  [[nodiscard]] auto unpack_impl(std::index_sequence<Is...>) -> std::tuple<response_slot<Ts>&...> {
    return std::tie(get<Is>()...);
  }

  template <std::size_t... Is>
  [[nodiscard]] auto unpack_impl(std::index_sequence<Is...>) const -> std::tuple<const response_slot<Ts>&...> {
    return std::tie(get<Is>()...);
  }

  template <std::size_t I, typename E>
  void set_error(E&& e) {
    std::get<I>(results_) = unexpected(response_error{std::forward<E>(e)});
  }
};

}  // namespace rediscoro


