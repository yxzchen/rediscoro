#pragma once

#include <xz/redis/resp3/detail/buffer.hpp>
#include <xz/redis/resp3/generator.hpp>
#include <xz/redis/resp3/node.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stack>
#include <string_view>

namespace xz::redis::resp3 {

using generator_type = generator<std::optional<std::vector<node_view>>>;

class parser {
 public:
  static constexpr std::string_view sep = "\r\n";
  static constexpr std::size_t default_max_depth = 64;

  explicit parser(std::size_t buffer_capacity = 8192, std::size_t max_depth = default_max_depth)
      : buffer_(buffer_capacity), max_depth_(max_depth) {}

  void feed(std::string_view data) { buffer_.feed(data); }
  std::span<char> prepare(std::size_t n) { return buffer_.prepare(n); }
  void commit(std::size_t n) { return buffer_.commit(n); }

  void reset() {
    buffer_.clear();
    while (!pending_.empty()) {
      pending_.pop();
    }
    ec_ = {};
  }

  std::error_code error() const { return ec_; }

  /** @brief Coroutine that yields parsed messages.
   *
   *  Yields std::nullopt when more data is needed, or a vector of node_views
   *  when a complete message is available.
   *
   *  @warning Lifetime guarantee: The returned node_views contain string_views
   *  that point into the parser's internal buffer. These views remain valid
   *  indefinitely and across feed() calls, until you call compact() or destroy
   *  the parser. The buffer never auto-compacts to preserve view validity.
   *
   *  Call compact() when you're done with all views to reclaim buffer memory.
   *  For long-term storage, use to_owning_node() or to_owning_nodes() from
   *  <xz/redis/resp3/node.hpp> to create deep copies.
   *
   *  @return A generator yielding optional vectors of node_views.
   */
  auto parse() -> generator_type;

 private:
  detail::buffer buffer_;
  std::stack<size_t> pending_;
  std::error_code ec_;
  std::size_t max_depth_;

  auto read_until_separator() noexcept -> std::optional<std::string_view>;
  auto read_bulk_data(std::size_t length, std::error_code& ec) noexcept -> std::optional<std::string_view>;

  void commit_elem() noexcept;
};

}  // namespace xz::redis::resp3
