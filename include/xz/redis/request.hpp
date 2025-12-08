#pragma once

#include <xz/redis/resp3/serializer.hpp>
#include <xz/redis/resp3/type.hpp>

#include <algorithm>
#include <string>
#include <tuple>

namespace xz::redis {

namespace detail {
auto is_subscribe(std::string_view cmd) -> bool;
}  // namespace detail

class request {
 public:
  [[nodiscard]] auto expected_responses() const noexcept -> std::size_t { return expected_responses_; };

  [[nodiscard]] auto payload() const noexcept -> std::string_view { return payload_; }

  void clear() {
    payload_.clear();
    expected_responses_ = 0;
  }

  void reserve(std::size_t new_cap = 0) { payload_.reserve(new_cap); }

  template <class... Ts>
  void push(std::string_view cmd, Ts const&... args) {
    auto constexpr pack_size = sizeof...(Ts);
    resp3::add_header(payload_, resp3::type::array, 1 + pack_size);
    resp3::add_bulk(payload_, cmd);
    resp3::add_bulk(payload_, std::tie(std::forward<Ts const&>(args)...));

    check_cmd(cmd);
  }

  template <class ForwardIterator>
  void push_range(std::string_view cmd, std::string_view key, ForwardIterator begin, ForwardIterator end,
                  typename std::iterator_traits<ForwardIterator>::value_type* = nullptr) {
    using value_type = typename std::iterator_traits<ForwardIterator>::value_type;

    if (begin == end) return;

    auto constexpr size = resp3::bulk_counter<value_type>::size;
    auto const distance = std::distance(begin, end);
    resp3::add_header(payload_, resp3::type::array, 2 + size * distance);
    resp3::add_bulk(payload_, cmd);
    resp3::add_bulk(payload_, key);

    for (; begin != end; ++begin) resp3::add_bulk(payload_, *begin);

    check_cmd(cmd);
  }

  template <class ForwardIterator>
  void push_range(std::string_view cmd, ForwardIterator begin, ForwardIterator end,
                  typename std::iterator_traits<ForwardIterator>::value_type* = nullptr) {
    using value_type = typename std::iterator_traits<ForwardIterator>::value_type;

    if (begin == end) return;

    auto constexpr size = resp3::bulk_counter<value_type>::size;
    auto const distance = std::distance(begin, end);
    resp3::add_header(payload_, resp3::type::array, 1 + size * distance);
    resp3::add_bulk(payload_, cmd);

    for (; begin != end; ++begin) resp3::add_bulk(payload_, *begin);

    check_cmd(cmd);
  }

  template <class Range>
  void push_range(std::string_view cmd, std::string_view key, Range const& range,
                  decltype(std::begin(range))* = nullptr) {
    push_range(cmd, key, std::begin(range), std::end(range));
  }

  template <class Range>
  void push_range(std::string_view cmd, Range const& range, decltype(std::cbegin(range))* = nullptr) {
    push_range(cmd, std::cbegin(range), std::cend(range));
  }

 private:
  void check_cmd(std::string_view cmd) {
    if (!detail::is_subscribe(cmd)) ++expected_responses_;
  }

  std::string payload_;
  std::size_t expected_responses_ = 0;
};

}  // namespace xz::redis
