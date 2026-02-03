#pragma once

#include <rediscoro/adapter/adapt.hpp>
#include <rediscoro/assert.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/response.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace rediscoro::detail {

template <typename T>
inline auto slot_from_message(resp3::message msg) -> response_slot<T> {
  if (msg.is<resp3::simple_error>()) {
    return unexpected(error_info{server_errc::redis_error,
                                 std::string{msg.as<resp3::simple_error>().message}});
  }
  if (msg.is<resp3::bulk_error>()) {
    return unexpected(
      error_info{server_errc::redis_error, std::string{msg.as<resp3::bulk_error>().message}});
  }

  auto r = adapter::adapt<T>(msg);
  if (!r) {
    auto e = std::move(r.error());
    error_info out{e.kind, e.to_string()};
    return unexpected(std::move(out));
  }
  return std::move(*r);
}

template <typename T>
inline auto slot_from_error(error_info err) -> response_slot<T> {
  return unexpected(std::move(err));
}

template <typename... Ts>
class response_builder {
 public:
  static constexpr std::size_t static_size = sizeof...(Ts);

  response_builder() = default;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return static_size; }
  [[nodiscard]] bool done() const noexcept { return next_index_ == static_size; }

  void accept(resp3::message msg) {
    REDISCORO_ASSERT(next_index_ < static_size);
    msg_dispatch_table()[next_index_](this, std::move(msg));
    next_index_ += 1;
  }

  void accept(error_info err) {
    REDISCORO_ASSERT(next_index_ < static_size);
    err_dispatch_table()[next_index_](this, std::move(err));
    next_index_ += 1;
  }

  response<Ts...> take_results() {
    REDISCORO_ASSERT(done());
    return response<Ts...>{take_results(std::index_sequence_for<Ts...>{})};
  }

 private:
  std::size_t next_index_{0};
  std::tuple<std::optional<response_slot<Ts>>...> results_{};

  template <std::size_t I>
  using nth_type = std::tuple_element_t<I, std::tuple<Ts...>>;

  template <std::size_t I>
  void set_slot(response_slot<nth_type<I>> slot) {
    auto& opt = std::get<I>(results_);
    REDISCORO_ASSERT(!opt.has_value());
    opt = std::move(slot);
  }

  template <std::size_t I, typename E>
  void set_error(E&& e) {
    static_assert(std::is_constructible_v<error_info, E>);
    set_slot<I>(unexpected(error_info{std::forward<E>(e)}));
  }

  template <std::size_t I>
  void set_from_message(resp3::message msg) {
    using T = nth_type<I>;
    set_slot<I>(slot_from_message<T>(std::move(msg)));
  }

  using msg_dispatch_fn = void (*)(response_builder*, resp3::message);
  using err_dispatch_fn = void (*)(response_builder*, error_info);

  template <std::size_t I>
  static void msg_dispatch(response_builder* self, resp3::message msg) {
    self->set_from_message<I>(std::move(msg));
  }

  template <std::size_t I>
  static void err_dispatch(response_builder* self, error_info err) {
    self->set_error<I>(std::move(err));
  }

  template <std::size_t... Is>
  static constexpr auto make_msg_table(std::index_sequence<Is...>)
    -> std::array<msg_dispatch_fn, static_size> {
    return {&msg_dispatch<Is>...};
  }

  template <std::size_t... Is>
  static constexpr auto make_err_table(std::index_sequence<Is...>)
    -> std::array<err_dispatch_fn, static_size> {
    return {&err_dispatch<Is>...};
  }

  static auto msg_dispatch_table() -> const std::array<msg_dispatch_fn, static_size>& {
    static constexpr auto table = make_msg_table(std::index_sequence_for<Ts...>{});
    return table;
  }

  static auto err_dispatch_table() -> const std::array<err_dispatch_fn, static_size>& {
    static constexpr auto table = make_err_table(std::index_sequence_for<Ts...>{});
    return table;
  }

  template <std::size_t... Is>
  auto take_results(std::index_sequence<Is...>) -> std::tuple<response_slot<Ts>...> {
    REDISCORO_ASSERT(((std::get<Is>(results_).has_value()) && ...));
    return std::tuple<response_slot<Ts>...>{std::move(*std::get<Is>(results_))...};
  }
};

template <typename T>
class dynamic_response_builder {
 public:
  explicit dynamic_response_builder(std::size_t expected_count) : expected_(expected_count) {
    results_.reserve(expected_count);
  }

  [[nodiscard]] std::size_t expected_count() const noexcept { return expected_; }
  [[nodiscard]] std::size_t size() const noexcept { return results_.size(); }
  [[nodiscard]] bool done() const noexcept { return results_.size() == expected_; }

  void accept(resp3::message msg) {
    REDISCORO_ASSERT(results_.size() < expected_);
    results_.push_back(slot_from_message<T>(std::move(msg)));
  }

  void accept(error_info err) {
    REDISCORO_ASSERT(results_.size() < expected_);
    results_.push_back(slot_from_error<T>(std::move(err)));
  }

  auto take_results() -> dynamic_response<T> {
    REDISCORO_ASSERT(done());
    return dynamic_response<T>{std::move(results_)};
  }

 private:
  std::size_t expected_{0};
  std::vector<response_slot<T>> results_{};
};

}  // namespace rediscoro::detail
