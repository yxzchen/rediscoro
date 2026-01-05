#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/expected.hpp>

#include <string_view>
#include <optional>
#include <cstdint>

namespace rediscoro::resp3 {

/// RESP3 protocol parser using generator coroutines
/// Supports resumable parsing - will co_yield when more data is needed
class parser {
public:
  explicit parser();

  /// Reset parser state (discards current parse coroutine if any)
  auto reset() -> void;

private:
  buffer buffer_;
};

}  // namespace rediscoro::resp3
