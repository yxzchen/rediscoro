#include <xz/io/io_context.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection.hpp>

#include <gtest/gtest.h>

using namespace xz::redis;
using namespace xz::io;
namespace redis_detail = xz::redis::detail;

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

  auto test_task = [&]() -> awaitable<void> {
    redis_detail::connection conn{ctx, cfg};
    try {
      co_await conn.run();
      EXPECT_TRUE(conn.is_running());
      conn.stop();
    } catch (const std::exception& e) {
      ADD_FAILURE() << "Run failed: " << e.what();
    }
  };

  co_spawn(ctx, test_task(), use_detached);
  ctx.run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
