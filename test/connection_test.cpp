#include <xz/io/io_context.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>

#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace xz::redis;
using namespace xz::io;
namespace test_util = xz::redis::test_util;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
  }

  config cfg;
};

TEST_F(ConnectionTest, RunBasic) {
  io_context ctx;

  connection conn{ctx, cfg};

  auto f = [&]() -> awaitable<void> {
    co_await conn.run();
    EXPECT_TRUE(conn.is_running());
  };

  auto res = test_util::run_io(ctx, f, [&]() { conn.stop(); });
  ASSERT_TRUE(res.ec == std::error_code{} && res.what.empty()) << (res.what.empty() ? "unknown error" : res.what);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
