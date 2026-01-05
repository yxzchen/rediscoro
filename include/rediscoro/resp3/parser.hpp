#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>

#include <system_error>
#include <memory>
#include <span>
#include <cstddef>

namespace rediscoro::resp3 {

enum class parse_status {
  need_more_data,
  ok,
  protocol_error,
};

struct parse_result {
  parse_status status{};
  std::error_code error{};  // only valid if status == protocol_error
};

namespace detail {

struct value_parser {
  virtual ~value_parser() = default;

  // Return:
  // - true  : parsing finished (either success, or ec is set to protocol error)
  // - false : need more data, out must not be modified by caller
  virtual auto parse(buffer& buf, message& out, std::error_code& ec) -> bool = 0;
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
  /// - ok: out is filled
  /// - need_more_data: out is not modified
  /// - protocol_error: parser enters failed() state, out is not modified
  auto parse_one(message& out) -> parse_result;

  [[nodiscard]] auto failed() const noexcept -> bool;

  /// Reset parser state and discard any buffered data.
  /// Intended for tests or explicit reuse.
  auto reset() -> void;

private:
  buffer buffer_;
  std::error_code last_error_{};

  enum class state {
    idle,
    parsing,
    failed,
  } state_{state::idle};

  std::unique_ptr<detail::value_parser> current_{};
};

}  // namespace rediscoro::resp3

#include <rediscoro/resp3/impl/parser.ipp>
