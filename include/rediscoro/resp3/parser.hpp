#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/expected.hpp>

#include <system_error>
#include <memory>
#include <span>
#include <cstddef>
#include <optional>

namespace rediscoro::resp3 {

namespace detail {

struct value_parser {
  virtual ~value_parser() = default;

  // Return:
  // - true  : parsing finished (either success, or err is set to protocol error)
  // - false : need more data, out must not be modified by caller
  virtual auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool = 0;
};

}  // namespace detail

/// RESP3 protocol parser (push-based, incremental)
class parser {
public:
  parser();

  /// Zero-copy input: reserve writable space and then commit written bytes.
  /// Typical usage:
  ///   auto w = p.prepare(n);
  ///   read(fd, w.data(), w.size());
  ///   p.commit(bytes_read);
  auto prepare(std::size_t min_size = 4096) -> std::span<char>;
  auto commit(std::size_t n) -> void;

  /// Try to parse exactly one complete RESP3 message.
  /// - success: returns message
  /// - error::needs_more: need more data (parser remains usable)
  /// - other errors: protocol error, parser enters failed() state
  auto parse_one() -> rediscoro::expected<message, error>;

  [[nodiscard]] auto failed() const noexcept -> bool;

  /// Reset parser state and discard any buffered data.
  /// Intended for tests or explicit reuse.
  auto reset() -> void;

private:
  buffer buffer_;
  bool failed_{false};
  error failed_error_{error::invalid_format};

  std::unique_ptr<detail::value_parser> current_{};
};

}  // namespace rediscoro::resp3

#include <rediscoro/resp3/impl/parser.ipp>
