#pragma once

#include <rediscoro/resp3/type.hpp>

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
    requires (sizeof...(Args) > 0)
  request(std::string_view cmd, Args&&... args) {
    push(cmd, std::forward<Args>(args)...);
  }

  /// Number of commands currently encoded in this request.
  [[nodiscard]] auto command_count() const noexcept -> std::size_t { return command_count_; }

  [[nodiscard]] auto empty() const noexcept -> bool { return command_count_ == 0; }

  /// The full RESP3 wire bytes for all queued commands (pipeline).
  [[nodiscard]] auto wire() const noexcept -> const std::string& { return wire_; }

  auto clear() -> void {
    wire_.clear();
    command_count_ = 0;
  }

  /// Append one complete command (argv form).
  auto push(std::initializer_list<std::string_view> argv) -> void {
    append_command_header(argv.size());
    for (auto sv : argv) {
      append_bulk_string(wire_, sv);
    }
    command_count_ += 1;
  }

  /// Append one complete command (argv form).
  auto push(std::span<const std::string_view> argv) -> void {
    append_command_header(argv.size());
    for (auto sv : argv) {
      append_bulk_string(wire_, sv);
    }
    command_count_ += 1;
  }

  /// Append one complete command (command + variadic args).
  template <typename... Args>
    requires (sizeof...(Args) >= 0)
  auto push(std::string_view cmd, Args&&... args) -> void {
    constexpr std::size_t n = 1 + sizeof...(Args);
    append_command_header(n);
    append_arg(wire_, cmd);
    (append_arg(wire_, std::forward<Args>(args)), ...);
    command_count_ += 1;
  }

  /// Append one complete command (single-token).
  auto push(std::string_view cmd) -> void {
    append_command_header(1);
    append_bulk_string(wire_, cmd);
    command_count_ += 1;
  }

private:
  std::string wire_{};
  std::size_t command_count_{0};

  static auto append_unsigned(std::string& out, std::size_t v) -> void {
    char buf[32]{};
    auto* first = buf;
    auto* last = buf + sizeof(buf);
    auto res = std::to_chars(first, last, v);
    if (res.ec != std::errc{}) {
      // Should never happen for size_t with enough buffer; keep behavior defined.
      return;
    }
    out.append(first, res.ptr);
  }

  auto append_command_header(std::size_t argc) -> void {
    wire_.push_back(rediscoro::resp3::type_to_code(rediscoro::resp3::type3::array));
    append_unsigned(wire_, argc);
    wire_.append("\r\n");
  }

  static auto append_bulk_string(std::string& out, std::string_view sv) -> void {
    out.push_back(rediscoro::resp3::type_to_code(rediscoro::resp3::type3::bulk_string));
    append_unsigned(out, sv.size());
    out.append("\r\n");
    out.append(sv.data(), sv.size());
    out.append("\r\n");
  }

  static auto append_arg(std::string& out, std::string_view sv) -> void {
    append_bulk_string(out, sv);
  }

  static auto append_arg(std::string& out, const char* s) -> void {
    if (s == nullptr) {
      append_bulk_string(out, std::string_view{});
    } else {
      append_bulk_string(out, std::string_view{s});
    }
  }

  static auto append_arg(std::string& out, const std::string& s) -> void {
    append_bulk_string(out, std::string_view{s});
  }

  static auto append_arg(std::string& out, std::string&& s) -> void {
    append_bulk_string(out, std::string_view{s});
  }

  template <typename T>
    requires (std::is_integral_v<std::remove_cvref_t<T>> && !std::is_same_v<std::remove_cvref_t<T>, bool>)
  static auto append_arg(std::string& out, T v) -> void {
    const auto tmp = std::to_string(v);
    append_bulk_string(out, std::string_view{tmp});
  }
};

}  // namespace rediscoro


