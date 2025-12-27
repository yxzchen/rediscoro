#pragma once

#include <rediscoro/error.hpp>
#include <rediscoro/adapter/result.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/node.hpp>

namespace rediscoro::adapter {

struct ignore {
  using result_type = result<ignore_t>;

  explicit ignore(result_type* r = nullptr) : result_(r) {}

  void on_msg(resp3::msg_view const& msg) {
    REDISCORO_ASSERT(!msg.empty());
    if (!result_) return;

    auto const& node = msg.front();
    if (is_error(node.data_type)) {
      *result_ = unexpected(error{std::string(node.value())});
    }
  }

 private:
  result_type* result_ = nullptr;
};

}  // namespace rediscoro::adapter
