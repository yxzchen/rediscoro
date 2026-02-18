#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/resp3/kind.hpp>

#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rediscoro {

/// A Redis request builder: describes what to send, and can serialize to RESP3 wire bytes.
///
/// - Input: command name + arguments (string / argv)
/// - Output: RESP3-encoded command (array of bulk strings)
class request {
 public:
  request() = default;

  /// Construct a request containing exactly one command.
  explicit request(std::string_view cmd) { push(cmd); }

  /// Construct a request containing exactly one command.
  request(std::initializer_list<std::string_view> argv) { push(argv); }

  /// Construct a request containing exactly one command.
  explicit request(std::span<const std::string_view> argv) { push(argv); }

  template <typename... Args>
    requires(sizeof...(Args) > 0)
  request(std::string_view cmd, Args&&... args) {
    push(cmd, std::forward<Args>(args)...);
  }

  [[nodiscard]] std::size_t reply_count() const noexcept { return command_count_; }
  [[nodiscard]] std::size_t command_count() const noexcept { return command_count_; }

  [[nodiscard]] bool empty() const noexcept { return command_count_ == 0; }

  /// The full RESP3 wire bytes for all queued commands (pipeline).
  [[nodiscard]] auto wire() const noexcept -> const std::string& { return wire_; }

  void clear() {
    wire_.clear();
    command_count_ = 0;
  }

  /// Append one complete command (argv form).
  void push(std::initializer_list<std::string_view> argv) {
    append_command_header(argv.size());
    for (auto sv : argv) {
      append_bulk_string(sv);
    }
    command_count_ += 1;
  }

  /// Append one complete command (argv form).
  void push(std::span<const std::string_view> argv) {
    append_command_header(argv.size());
    for (auto sv : argv) {
      append_bulk_string(sv);
    }
    command_count_ += 1;
  }

  /// Append one complete command (command + variadic args).
  template <typename... Args>
  void push(std::string_view cmd, Args&&... args) {
    constexpr std::size_t n = 1 + sizeof...(Args);
    append_command_header(n);
    append_arg(cmd);
    (append_arg(std::forward<Args>(args)), ...);
    command_count_ += 1;
  }

  /// Append one complete command (single-token).
  void push(std::string_view cmd) {
    append_command_header(1);
    append_bulk_string(cmd);
    command_count_ += 1;
  }

 private:
  std::string wire_{};
  std::size_t command_count_{0};

  void append_unsigned(std::size_t v) {
    char buf[32]{};
    auto* first = buf;
    auto* last = buf + sizeof(buf);
    auto res = std::to_chars(first, last, v);
    REDISCORO_ASSERT(res.ec == std::errc{});
    wire_.append(first, res.ptr);
  }

  void append_command_header(std::size_t argc) {
    wire_.push_back(resp3::kind_to_prefix(resp3::kind::array));
    append_unsigned(argc);
    wire_.append("\r\n");
  }

  void append_bulk_string(std::string_view sv) {
    wire_.push_back(resp3::kind_to_prefix(resp3::kind::bulk_string));
    append_unsigned(sv.size());
    wire_.append("\r\n");
    wire_.append(sv.data(), sv.size());
    wire_.append("\r\n");
  }

  void append_arg(std::string_view sv) { append_bulk_string(sv); }

  void append_arg(const char* s) {
    append_bulk_string(s != nullptr ? std::string_view{s} : std::string_view{});
  }

  void append_arg(const std::string& s) { append_bulk_string(std::string_view{s}); }

  template <typename T>
    requires(std::is_integral_v<std::remove_cvref_t<T>> &&
             !std::is_same_v<std::remove_cvref_t<T>, bool>)
  void append_arg(T v) {
    char buf[64]{};
    auto* first = buf;
    auto* last = buf + sizeof(buf);
    auto res = std::to_chars(first, last, v);
    REDISCORO_ASSERT(res.ec == std::errc{});
    append_bulk_string(std::string_view{first, static_cast<std::size_t>(res.ptr - first)});
  }
};

}  // namespace rediscoro
