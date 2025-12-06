/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/adapter/adapter.hpp>
#include <redisus/error.hpp>
#include <redisus/resp3/node.hpp>

#include <charconv>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace redisus::adapter {

// Convert RESP3 node to string
inline void from_node(std::string& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  if (node.data_type == resp3::type3::null) {
    result.clear();
    return;
  }

  result = node.value();
}

// Convert RESP3 node to string_view
inline void from_node(std::string_view& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  if (node.data_type == resp3::type3::null) {
    result = std::string_view{};
    return;
  }

  result = node.value();
}

// Convert RESP3 node to integral type
template <class T>
auto from_node(T& result, resp3::node_view const& node, std::error_code& ec) -> std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, void> {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (res.ec != std::errc{}) {
    ec = error::not_a_number;
  }
}

// Convert RESP3 node to double
inline void from_node(double& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (res.ec != std::errc()) {
    ec = error::not_a_double;
  }
}

// Convert RESP3 node to bool
inline void from_node(bool& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  if (sv.size() == 1) {
    if (sv[0] == 't' || sv[0] == '1') {
      result = true;
      return;
    } else if (sv[0] == 'f' || sv[0] == '0') {
      result = false;
      return;
    }
  }

  ec = error::expects_resp3_simple_type;
}

// Ignore adapter - discards all parsed values
// Useful for validation or when you only care about parsing success
class ignore_adapter {
 public:
  ignore_adapter() = default;

  void on_node(resp3::node_view const& node, std::error_code& ec) {
    // Intentionally does nothing - just validates parsing
    (void)node;
    (void)ec;
  }
};

// Global ignore instance for convenience
inline ignore_adapter ignore;

// Simple adapter for a single value
template <class T>
class simple_adapter {
 private:
  T* result_;

 public:
  explicit simple_adapter(T& result) : result_(&result) {
  }

  void on_node(resp3::node_view const& node, std::error_code& ec) {
    from_node(*result_, node, ec);
  }
};

// Adapter for std::vector<T>
template <class T>
class vector_adapter {
 private:
  std::vector<T>* result_;
  int depth_ = -1;

 public:
  explicit vector_adapter(std::vector<T>& result) : result_(&result) {
  }

  void on_node(resp3::node_view const& node, std::error_code& ec) {
    if (resp3::is_aggregate(node.data_type)) {
      if (depth_ == -1) {
        // First aggregate - this is the array itself
        depth_ = 0;
        result_->clear();
        result_->reserve(node.aggregate_size());
      } else {
        // Nested aggregates not supported for simple vector adapter
        ec = error::nested_aggregate_not_supported;
      }
    } else {
      // Simple element
      if (depth_ >= 0) {
        T value{};
        from_node(value, node, ec);
        if (!ec) {
          result_->push_back(std::move(value));
        }
      }
    }
  }
};

// Adapter for std::optional<T>
template <class T>
class optional_adapter {
 private:
  std::optional<T>* result_;
  simple_adapter<T> inner_;
  T temp_;

 public:
  explicit optional_adapter(std::optional<T>& result) : result_(&result), inner_(temp_) {
    result_->reset();
  }

  void on_node(resp3::node_view const& node, std::error_code& ec) {
    if (node.data_type == resp3::type3::null) {
      result_->reset();
      return;
    }

    inner_.on_node(node, ec);
    if (!ec) {
      *result_ = std::move(temp_);
    }
  }
};

// Adapter for std::map<K, V>
template <class K, class V>
class map_adapter {
 private:
  std::map<K, V>* result_;
  int depth_ = -1;
  bool on_key_ = true;
  K current_key_;

 public:
  explicit map_adapter(std::map<K, V>& result) : result_(&result) {
  }

  void on_node(resp3::node_view const& node, std::error_code& ec) {
    if (resp3::is_aggregate(node.data_type)) {
      if (depth_ == -1) {
        // First aggregate - this is the map itself
        depth_ = 0;
        result_->clear();
        on_key_ = true;
      } else {
        // Nested aggregates not supported for simple map adapter
        ec = error::nested_aggregate_not_supported;
      }
    } else {
      // Simple element - alternates between key and value
      if (depth_ >= 0) {
        if (on_key_) {
          from_node(current_key_, node, ec);
          on_key_ = false;
        } else {
          V value{};
          from_node(value, node, ec);
          if (!ec) {
            (*result_)[current_key_] = std::move(value);
          }
          on_key_ = true;
        }
      }
    }
  }
};

// Helper function to create adapters
template <class T>
auto make_adapter(T& result) {
  return simple_adapter<T>(result);
}

template <class T>
auto make_adapter(std::vector<T>& result) {
  return vector_adapter<T>(result);
}

template <class T>
auto make_adapter(std::optional<T>& result) {
  return optional_adapter<T>(result);
}

template <class K, class V>
auto make_adapter(std::map<K, V>& result) {
  return map_adapter<K, V>(result);
}

}  // namespace redisus::adapter
