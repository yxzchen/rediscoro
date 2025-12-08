#pragma once

#include <redisus/adapter/detail/convert.hpp>
#include <redisus/resp3/node.hpp>

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

namespace redisus::adapter::detail {

template <class Result>
class general_aggregate {
 private:
  Result* result_;

 public:
  explicit general_aggregate(Result* c = nullptr) : result_(c) {}

  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
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
      ec = redisus::error::expects_resp3_simple_type;
      return;
    }

    if (is_aggregate(msg.front().data_type)) {
      ec = redisus::error::expects_resp3_simple_type;
      return;
    }

    from_bulk(result, msg.front(), ec);
  }
};

template <class Result>
class set_impl {
 public:
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (header.data_type != resp3::type3::set && header.data_type != resp3::type3::array) {
      ec = redisus::error::expects_resp3_set;
      return;
    }

    auto expected_count = header.aggregate_size();
    if (msg.size() != expected_count + 1) {
      ec = redisus::error::incompatible_size;
      return;
    }

    typename Result::iterator hint = result.end();
    for (std::size_t i = 1; i < msg.size(); ++i) {
      auto const& node = msg[i];

      if (node.is_aggregate_node()) {
        ec = redisus::error::nested_aggregate_not_supported;
        return;
      }

      typename Result::key_type obj;
      from_bulk(obj, node, ec);
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

    if (header.data_type != resp3::type3::map && header.data_type != resp3::type3::attribute) {
      ec = redisus::error::expects_resp3_map;
      return;
    }

    auto expected_count = header.aggregate_size() * 2;
    if (msg.size() != expected_count + 1) {
      ec = redisus::error::incompatible_size;
      return;
    }

    typename Result::iterator hint = result.end();
    for (std::size_t i = 1; i < msg.size(); i += 2) {
      auto const& key_node = msg[i];
      auto const& val_node = msg[i + 1];

      if (key_node.is_aggregate_node() || val_node.is_aggregate_node()) {
        ec = redisus::error::nested_aggregate_not_supported;
        return;
      }

      typename Result::key_type key;
      from_bulk(key, key_node, ec);
      if (ec) return;

      typename Result::mapped_type value;
      from_bulk(value, val_node, ec);
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

    if (header.data_type != resp3::type3::set && header.data_type != resp3::type3::array &&
        header.data_type != resp3::type3::push) {
      ec = redisus::error::expects_resp3_aggregate;
      return;
    }

    auto expected_count = header.aggregate_size();
    if (msg.size() != expected_count + 1) {
      ec = redisus::error::incompatible_size;
      return;
    }

    result.reserve(result.size() + expected_count);
    for (std::size_t i = 1; i < msg.size(); ++i) {
      auto const& node = msg[i];

      if (node.is_aggregate_node()) {
        ec = redisus::error::nested_aggregate_not_supported;
        return;
      }

      typename Result::value_type obj;
      from_bulk(obj, node, ec);
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
      ec = redisus::error::expects_resp3_aggregate;
      return;
    }

    auto expected_count = header.aggregate_size();
    if (msg.size() != expected_count + 1) {
      ec = redisus::error::incompatible_size;
      return;
    }

    if (result.size() != expected_count) {
      ec = redisus::error::incompatible_size;
      return;
    }

    for (std::size_t i = 1; i < msg.size(); ++i) {
      auto const& node = msg[i];

      if (node.is_aggregate_node()) {
        ec = redisus::error::nested_aggregate_not_supported;
        return;
      }

      from_bulk(result[i - 1], node, ec);
      if (ec) return;
    }
  }
};

template <class Result>
struct list_impl {
  void on_msg(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    auto const& header = msg.front();

    if (header.data_type != resp3::type3::set && header.data_type != resp3::type3::array &&
        header.data_type != resp3::type3::push) {
      ec = redisus::error::expects_resp3_aggregate;
      return;
    }

    auto expected_count = header.aggregate_size();
    if (msg.size() != expected_count + 1) {
      ec = redisus::error::incompatible_size;
      return;
    }

    for (std::size_t i = 1; i < msg.size(); ++i) {
      auto const& node = msg[i];

      if (node.is_aggregate_node()) {
        ec = redisus::error::nested_aggregate_not_supported;
        return;
      }

      typename Result::value_type obj;
      from_bulk(obj, node, ec);
      if (ec) return;
      result.push_back(std::move(obj));
    }
  }
};

template <class T>
struct impl_map {
  using type = simple_impl<T>;
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

}  // namespace redisus::adapter::detail
