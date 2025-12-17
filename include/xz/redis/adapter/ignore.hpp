#pragma once

#include <xz/redis/error.hpp>
#include <xz/redis/adapter/result.hpp>
#include <xz/redis/expected.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis::adapter {

struct ignore {
  using result_type = result<ignore_t>;

  explicit ignore(result_type* r = nullptr) : result_(r) {}

  void on_msg(resp3::msg_view const& msg) {
    REDISXZ_ASSERT(!msg.empty());
    if (!result_) return;

    auto const& node = msg.front();
    if (is_error(node.data_type)) {
      *result_ = unexpected(error{std::string(node.value())});
    }
  }

 private:
  result_type* result_ = nullptr;
};

}  // namespace xz::redis::adapter
