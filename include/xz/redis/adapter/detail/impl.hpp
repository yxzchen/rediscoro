#pragma once

#include <xz/redis/adapter/detail/convert.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/node.hpp>

#include <array>
#include <charconv>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xz::redis::adapter::detail {

inline bool validate_aggregate(resp3::msg_view const& msg, std::size_t expected_element_count, std::error_code& ec) {
  auto const& header = msg.front();

  if (!is_aggregate(header.data_type)) {
    ec = xz::redis::error::expects_resp3_aggregate;
    return false;
  }

  auto multiplier = element_multiplicity(header.data_type);
  if (msg.size() != header.aggregate_size() * multiplier + 1) {
    ec = xz::redis::error::incompatible_size;
    return false;
  }

  return true;
}

inline bool has_nested_aggregates(resp3::msg_view const& msg, std::size_t start_idx, std::error_code& ec) {
  for (std::size_t i = start_idx; i < msg.size(); ++i) {
    if (msg[i].is_aggregate_node()) {
      ec = xz::redis::error::nested_aggregate_not_supported;
      return true;
    }
  }
  return false;
}

template <class Result>
class general_aggregate {
 private:
  Result* result_;

 public:
  explicit general_aggregate(Result* c = nullptr) : result_(c) {}

  void on_msg(resp3::msg_view const& msg) {
    auto& vec = result_->value();
    vec.reserve(vec.size() + msg.size());
    for (auto const& node_view : msg) {
      vec.push_back(resp3::to_owning_node(node_view));
    }
  }
};

template <class Result>
class simple_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    if (msg.size() > 1) {
      ec = xz::redis::error::expects_resp3_simple_type;
      return;
    }

    if (is_aggregate(msg.front().data_type)) {
      ec = xz::redis::error::expects_resp3_simple_type;
      return;
    }

    from_bulk(result, msg.front(), ec);
  }
};

struct ignore_impl {
  void on_msg(ignore_t& result, resp3::msg_view const& msg, std::error_code& ec) {}
};

template <class Result>
class set_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (header.data_type != resp3::type3::set) {
      ec = xz::redis::error::expects_resp3_set;
      return;
    }

    if (msg.size() != header.aggregate_size() + 1) {
      ec = xz::redis::error::incompatible_size;
      return;
    }

    if (has_nested_aggregates(msg, 1, ec)) return;

    typename Result::iterator hint = result.end();
    for (std::size_t i = 1; i < msg.size(); ++i) {
      typename Result::key_type obj;
      from_bulk(obj, msg[i], ec);
      if (ec) return;
      hint = result.insert(hint, std::move(obj));
    }
  }
};

template <class Result>
class map_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (!is_map_like(header.data_type)) {
      ec = xz::redis::error::expects_resp3_map;
      return;
    }

    if (msg.size() != header.aggregate_size() * 2 + 1) {
      ec = xz::redis::error::incompatible_size;
      return;
    }

    if (has_nested_aggregates(msg, 1, ec)) return;

    typename Result::iterator hint = result.end();
    for (std::size_t i = 1; i < msg.size(); i += 2) {
      typename Result::key_type key;
      from_bulk(key, msg[i], ec);
      if (ec) return;

      typename Result::mapped_type value;
      from_bulk(value, msg[i + 1], ec);
      if (ec) return;

      hint = result.insert(hint, {std::move(key), std::move(value)});
    }
  }
};

template <class Result>
class vector_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (!is_array_like(header.data_type)) {
      ec = xz::redis::error::expects_resp3_aggregate;
      return;
    }

    if (msg.size() != header.aggregate_size() + 1) {
      ec = xz::redis::error::incompatible_size;
      return;
    }

    if (has_nested_aggregates(msg, 1, ec)) return;

    result.reserve(result.size() + header.aggregate_size());
    for (std::size_t i = 1; i < msg.size(); ++i) {
      typename Result::value_type obj;
      from_bulk(obj, msg[i], ec);
      if (ec) return;
      result.push_back(std::move(obj));
    }
  }
};

template <class Result>
class array_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (header.data_type != resp3::type3::array) {
      ec = xz::redis::error::expects_resp3_aggregate;
      return;
    }

    auto expected_count = header.aggregate_size();
    if (msg.size() != expected_count + 1 || result.size() != expected_count) {
      ec = xz::redis::error::incompatible_size;
      return;
    }

    if (has_nested_aggregates(msg, 1, ec)) return;

    for (std::size_t i = 1; i < msg.size(); ++i) {
      from_bulk(result[i - 1], msg[i], ec);
      if (ec) return;
    }
  }
};

template <class Result>
struct list_impl {
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (!is_array_like(header.data_type)) {
      ec = xz::redis::error::expects_resp3_aggregate;
      return;
    }

    if (msg.size() != header.aggregate_size() + 1) {
      ec = xz::redis::error::incompatible_size;
      return;
    }

    if (has_nested_aggregates(msg, 1, ec)) return;

    for (std::size_t i = 1; i < msg.size(); ++i) {
      typename Result::value_type obj;
      from_bulk(obj, msg[i], ec);
      if (ec) return;
      result.push_back(std::move(obj));
    }
  }
};

template <class T>
struct impl_map {
  using type = simple_impl<T>;
};

template <>
struct impl_map<ignore_t> {
  using type = ignore_impl;
};

template <class Key, class Compare, class Allocator>
struct impl_map<std::set<Key, Compare, Allocator>> {
  using type = set_impl<std::set<Key, Compare, Allocator>>;
};

template <class Key, class Compare, class Allocator>
struct impl_map<std::multiset<Key, Compare, Allocator>> {
  using type = set_impl<std::multiset<Key, Compare, Allocator>>;
};

template <class Key, class Hash, class KeyEqual, class Allocator>
struct impl_map<std::unordered_set<Key, Hash, KeyEqual, Allocator>> {
  using type = set_impl<std::unordered_set<Key, Hash, KeyEqual, Allocator>>;
};

template <class Key, class Hash, class KeyEqual, class Allocator>
struct impl_map<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>> {
  using type = set_impl<std::unordered_multiset<Key, Hash, KeyEqual, Allocator>>;
};

template <class Key, class T, class Compare, class Allocator>
struct impl_map<std::map<Key, T, Compare, Allocator>> {
  using type = map_impl<std::map<Key, T, Compare, Allocator>>;
};

template <class Key, class T, class Compare, class Allocator>
struct impl_map<std::multimap<Key, T, Compare, Allocator>> {
  using type = map_impl<std::multimap<Key, T, Compare, Allocator>>;
};

template <class Key, class Hash, class KeyEqual, class Allocator>
struct impl_map<std::unordered_map<Key, Hash, KeyEqual, Allocator>> {
  using type = map_impl<std::unordered_map<Key, Hash, KeyEqual, Allocator>>;
};

template <class Key, class Hash, class KeyEqual, class Allocator>
struct impl_map<std::unordered_multimap<Key, Hash, KeyEqual, Allocator>> {
  using type = map_impl<std::unordered_multimap<Key, Hash, KeyEqual, Allocator>>;
};

template <class T, class Allocator>
struct impl_map<std::vector<T, Allocator>> {
  using type = vector_impl<std::vector<T, Allocator>>;
};

template <class T, std::size_t N>
struct impl_map<std::array<T, N>> {
  using type = array_impl<std::array<T, N>>;
};

template <class T, class Allocator>
struct impl_map<std::list<T, Allocator>> {
  using type = list_impl<std::list<T, Allocator>>;
};

template <class T, class Allocator>
struct impl_map<std::deque<T, Allocator>> {
  using type = list_impl<std::deque<T, Allocator>>;
};

}  // namespace xz::redis::adapter::detail
