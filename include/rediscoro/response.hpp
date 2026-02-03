#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/expected.hpp>

#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rediscoro {

namespace detail {
template <typename... Ts>
class response_builder;

template <typename T>
class dynamic_response_builder;
}  // namespace detail

template <typename T>
using response_slot = expected<T, error_info>;

/// Typed response for a pipeline (compile-time sized, heterogenous slots).
template <typename... Ts>
class response {
 public:
  static constexpr std::size_t static_size = sizeof...(Ts);

  response() = delete;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return static_size; }
  [[nodiscard]] static constexpr bool empty() noexcept { return static_size == 0; }

  template <std::size_t I>
  [[nodiscard]] auto get() -> response_slot<std::tuple_element_t<I, std::tuple<Ts...>>>& {
    return std::get<I>(results_);
  }

  template <std::size_t I>
  [[nodiscard]] auto get() const
    -> const response_slot<std::tuple_element_t<I, std::tuple<Ts...>>>& {
    return std::get<I>(results_);
  }

  /// Convenience: unpack as tuple of references for structured bindings.
  [[nodiscard]] auto unpack() -> std::tuple<response_slot<Ts>&...> {
    return unpack_impl(std::index_sequence_for<Ts...>{});
  }

  [[nodiscard]] auto unpack() const -> std::tuple<const response_slot<Ts>&...> {
    return unpack_impl(std::index_sequence_for<Ts...>{});
  }

 private:
  std::tuple<response_slot<Ts>...> results_{};

  template <std::size_t... Is>
  [[nodiscard]] auto unpack_impl(std::index_sequence<Is...>) -> std::tuple<response_slot<Ts>&...> {
    return std::tie(get<Is>()...);
  }

  template <std::size_t... Is>
  [[nodiscard]] auto unpack_impl(std::index_sequence<Is...>) const
    -> std::tuple<const response_slot<Ts>&...> {
    return std::tie(get<Is>()...);
  }

  template <typename...>
  friend class detail::response_builder;

  explicit response(std::tuple<response_slot<Ts>...>&& results) : results_(std::move(results)) {}
};

/// Runtime-sized response where all slots have the same value type T.
template <typename T>
class dynamic_response {
 public:
  dynamic_response() = default;

  [[nodiscard]] std::size_t size() const noexcept { return results_.size(); }
  [[nodiscard]] bool empty() const noexcept { return results_.empty(); }

  [[nodiscard]] const response_slot<T>& operator[](std::size_t i) const { return results_[i]; }
  [[nodiscard]] response_slot<T>& operator[](std::size_t i) { return results_[i]; }

  [[nodiscard]] const response_slot<T>& at(std::size_t i) const { return results_.at(i); }
  [[nodiscard]] response_slot<T>& at(std::size_t i) { return results_.at(i); }

  [[nodiscard]] auto begin() const noexcept { return results_.begin(); }
  [[nodiscard]] auto end() const noexcept { return results_.end(); }

 private:
  std::vector<response_slot<T>> results_{};

  template <typename>
  friend class detail::dynamic_response_builder;

  explicit dynamic_response(std::vector<response_slot<T>>&& results)
      : results_(std::move(results)) {}
};

}  // namespace rediscoro
