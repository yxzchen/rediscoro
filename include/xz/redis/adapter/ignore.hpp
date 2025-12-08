#pragma once

#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis::adapter {

struct ignore {
  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    // clang-format off
    switch (msg.front().data_type) {
      case resp3::type3::simple_error: ec = xz::redis::error::resp3_simple_error; break;
      case resp3::type3::blob_error:   ec = xz::redis::error::resp3_blob_error; break;
      case resp3::type3::null:         ec = xz::redis::error::resp3_null; break;
      default:                         ;
    }
    // clang-format on
  }
};

}  // namespace xz::redis::adapter
