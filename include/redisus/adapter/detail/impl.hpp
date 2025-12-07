#pragma once

#include <include/redisus/adapter/detail/convert.hpp>

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

// template <class Result>
// class general_aggregate {
//  private:
//   Result* result_;

//  public:
//   explicit general_aggregate(Result* c = nullptr) : result_(c) {}

//
//

//   template <class String>
//   void on_node(resp3::basic_node<String> const& nd, system::error_code&) {
//     BOOST_ASSERT_MSG(!!result_, "Unexpected null pointer");
//     switch (nd.data_type) {
//       case resp3::type::blob_error:
//       case resp3::type::simple_error:
//         *result_ = error{nd.data_type, std::string{std::cbegin(nd.value), std::cend(nd.value)}};
//         break;
//       default:
//         if (result_->has_value()) {
//           (**result_).push_back(
//               {nd.data_type, nd.aggregate_size, nd.depth, std::string{std::cbegin(nd.value), std::cend(nd.value)}});
//         }
//     }
//   }
// };

// template <class Node>
// class general_simple {
//  private:
//   Node* result_;

//  public:
//   explicit general_simple(Node* t = nullptr) : result_(t) {}

//
//

//   template <class String>
//   void on_node(resp3::basic_node<String> const& nd, system::error_code&) {
//     BOOST_ASSERT_MSG(!!result_, "Unexpected null pointer");
//     switch (nd.data_type) {
//       case resp3::type::blob_error:
//       case resp3::type::simple_error:
//         *result_ = error{nd.data_type, std::string{std::cbegin(nd.value), std::cend(nd.value)}};
//         break;
//       default:
//         result_->value().data_type = nd.data_type;
//         result_->value().aggregate_size = nd.aggregate_size;
//         result_->value().depth = nd.depth;
//         result_->value().value.assign(nd.value.data(), nd.value.size());
//     }
//   }
// };

template <class Result>
class simple_impl {
 public:
  template <class String>
  void on_node(Result& result, resp3::msg_view const& msg, std::error_code& ec) {
    if (msg.size() != 1) {
      ec = redisus::error::expects_resp3_simple_type;
      return;
    }

    if (is_aggregate(msg.front().data_type)) {
      ec = redis::error::expects_resp3_simple_type;
      return;
    }

    from_bulk(result, msg.front(), ec);
  }
};

// template <class Result>
// class set_impl {
//  private:
//   typename Result::iterator hint_;

//  public:
//   template <class String>
//   void on_node(Result& result, resp3::basic_node<String> const& nd, system::error_code& ec) {
//     if (is_aggregate(nd.data_type)) {
//       if (nd.data_type != resp3::type::set) ec = redis::error::expects_resp3_set;
//       return;
//     }

//     BOOST_ASSERT(nd.aggregate_size == 1);

//     if (nd.depth < 1) {
//       ec = redis::error::expects_resp3_set;
//       return;
//     }

//     typename Result::key_type obj;
//     from_bulk(obj, nd, ec);
//     hint_ = result.insert(hint_, std::move(obj));
//   }
// };

// template <class Result>
// class map_impl {
//  private:
//   typename Result::iterator current_;
//   bool on_key_ = true;

//  public:
//   template <class String>
//   void on_node(Result& result, resp3::basic_node<String> const& nd, system::error_code& ec) {
//     if (is_aggregate(nd.data_type)) {
//       if (element_multiplicity(nd.data_type) != 2) ec = redis::error::expects_resp3_map;
//       return;
//     }

//     BOOST_ASSERT(nd.aggregate_size == 1);

//     if (nd.depth < 1) {
//       ec = redis::error::expects_resp3_map;
//       return;
//     }

//     if (on_key_) {
//       typename Result::key_type obj;
//       from_bulk(obj, nd, ec);
//       current_ = result.insert(current_, {std::move(obj), {}});
//     } else {
//       typename Result::mapped_type obj;
//       from_bulk(obj, nd, ec);
//       current_->second = std::move(obj);
//     }

//     on_key_ = !on_key_;
//   }
// };

// template <class Result>
// class vector_impl {
//  public:
//   template <class String>
//   void on_node(Result& result, resp3::basic_node<String> const& nd, system::error_code& ec) {
//     if (is_aggregate(nd.data_type)) {
//       auto const m = element_multiplicity(nd.data_type);
//       result.reserve(result.size() + m * nd.aggregate_size);
//     } else {
//       result.push_back({});
//       from_bulk(result.back(), nd, ec);
//     }
//   }
// };

// template <class Result>
// class array_impl {
//  private:
//   int i_ = -1;

//  public:
//   template <class String>
//   void on_node(Result& result, resp3::basic_node<String> const& nd, system::error_code& ec) {
//     if (is_aggregate(nd.data_type)) {
//       if (i_ != -1) {
//         ec = redis::error::nested_aggregate_not_supported;
//         return;
//       }

//       if (result.size() != nd.aggregate_size * element_multiplicity(nd.data_type)) {
//         ec = redis::error::incompatible_size;
//         return;
//       }
//     } else {
//       if (i_ == -1) {
//         ec = redis::error::expects_resp3_aggregate;
//         return;
//       }

//       BOOST_ASSERT(nd.aggregate_size == 1);
//       from_bulk(result.at(i_), nd, ec);
//     }

//     ++i_;
//   }
// };

// template <class Result>
// struct list_impl {
//   template <class String>
//   void on_node(Result& result, resp3::basic_node<String> const& nd, system::error_code& ec) {
//     if (!is_aggregate(nd.data_type)) {
//       BOOST_ASSERT(nd.aggregate_size == 1);
//       if (nd.depth < 1) {
//         ec = redis::error::expects_resp3_aggregate;
//         return;
//       }

//       result.push_back({});
//       from_bulk(result.back(), nd, ec);
//     }
//   }
// };

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
