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

TEST_F(ConnectionTest, ConnectBasic) {
  io_context ctx;

  auto test_task = [&]() -> awaitable<void> {
    redis_detail::connection conn{ctx, cfg};
    try {
      co_await conn.connect();
      EXPECT_TRUE(conn.is_connected());
      conn.close();
    } catch (const std::exception& e) {
      ADD_FAILURE() << "Connect failed: " << e.what();
    }
  };

  co_spawn(ctx, test_task(), use_detached);
  ctx.run();
}

TEST_F(ConnectionTest, ConnectWithClientName) {
  io_context ctx;
  cfg.client_name = "test-client";

  auto test_task = [&]() -> awaitable<void> {
    redis_detail::connection conn{ctx, cfg};
    try {
      co_await conn.connect();
      EXPECT_TRUE(conn.is_connected());
      conn.close();
    } catch (const std::exception& e) {
      ADD_FAILURE() << "Connect failed: " << e.what();
    }
  };

  co_spawn(ctx, test_task(), use_detached);
  ctx.run();
}

TEST_F(ConnectionTest, ConnectWithDatabase) {
  io_context ctx;
  cfg.database = 1;

  auto test_task = [&]() -> awaitable<void> {
    redis_detail::connection conn{ctx, cfg};
    try {
      co_await conn.connect();
      EXPECT_TRUE(conn.is_connected());
      conn.close();
    } catch (const std::exception& e) {
      ADD_FAILURE() << "Connect failed: " << e.what();
    }
  };

  co_spawn(ctx, test_task(), use_detached);
  ctx.run();
}

TEST_F(ConnectionTest, ConnectWithAll) {
  io_context ctx;
  cfg.database = 2;
  cfg.client_name = "full-test-client";

  auto test_task = [&]() -> awaitable<void> {
    redis_detail::connection conn{ctx, cfg};
    try {
      co_await conn.connect();
      EXPECT_TRUE(conn.is_connected());
      conn.close();
    } catch (const std::exception& e) {
      ADD_FAILURE() << "Connect failed: " << e.what();
    }
  };

  co_spawn(ctx, test_task(), use_detached);
  ctx.run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
