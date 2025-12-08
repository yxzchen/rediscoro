#pragma once

#include <redisus/error.hpp>
#include <redisus/resp3/node.hpp>

namespace redisus::adapter {

struct ignore {
  void on_msg(resp3::msg_view const& msg, std::error_code& ec) {
    // clang-format off
      switch (msg.front().data_type) {
        case resp3::type3::simple_error: ec = redisus::error::resp3_simple_error; break;
        case resp3::type3::blob_error:   ec = redisus::error::resp3_blob_error; break;
        case resp3::type3::null:         ec = redisus::error::resp3_null; break;
        default:                        ;
      }
    // clang-format on
  }
};

}  // namespace redisus::adapter
