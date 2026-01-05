#pragma once

#include <rediscoro/adapter/adapt.hpp>
#include <rediscoro/assert.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/response.hpp>

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace rediscoro {

template <typename... Ts>
class response_builder {
public:
  static constexpr std::size_t static_size = sizeof...(Ts);

  response_builder() = default;

  [[nodiscard]] static constexpr std::size_t size() noexcept { return static_size; }
  [[nodiscard]] bool done() const noexcept { return next_index_ == static_size; }
  [[nodiscard]] std::size_t next_index() const noexcept { return next_index_; }

  void accept(resp3::message msg) {
    set_next_from_message(std::move(msg));
    next_index_ += 1;
  }

  void accept(resp3::error e) {
    set_next_resp3_error(e);
    next_index_ += 1;
  }

  response<Ts...> finish() {
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
    set_slot<I>(unexpected(response_error{std::forward<E>(e)}));
  }

  template <std::size_t I>
  void set_from_message(resp3::message msg) {
    using T = nth_type<I>;

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
    set_slot<I>(std::move(*r));
  }

  template <std::size_t I>
  void set_resp3_error(resp3::error e) {
    set_error<I>(e);
  }

  void set_next_from_message(resp3::message msg) {
    REDISCORO_ASSERT(next_index_ < static_size);
    set_next_from_message_impl<0>(std::move(msg));
  }

  template <std::size_t I>
  void set_next_from_message_impl(resp3::message msg) {
    if constexpr (I < static_size) {
      if (next_index_ == I) {
        set_from_message<I>(std::move(msg));
        return;
      }
      set_next_from_message_impl<I + 1>(std::move(msg));
    } else {
      REDISCORO_ASSERT(false);
    }
  }

  void set_next_resp3_error(resp3::error e) {
    REDISCORO_ASSERT(next_index_ < static_size);
    set_next_resp3_error_impl<0>(e);
  }

  template <std::size_t I>
  void set_next_resp3_error_impl(resp3::error e) {
    if constexpr (I < static_size) {
      if (next_index_ == I) {
        set_resp3_error<I>(e);
        return;
      }
      set_next_resp3_error_impl<I + 1>(e);
    } else {
      REDISCORO_ASSERT(false);
    }
  }

  template <std::size_t... Is>
  auto take_results(std::index_sequence<Is...>) -> std::tuple<response_slot<Ts>...> {
    REDISCORO_ASSERT(((std::get<Is>(results_).has_value()) && ...));
    return std::tuple<response_slot<Ts>...>{std::move(*std::get<Is>(results_))...};
  }
};

}  // namespace rediscoro


