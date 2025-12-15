#include <xz/io/io_context.hpp>
#include <xz/io/when_all.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace xz::io;
using namespace xz::redis;
namespace redis_detail = xz::redis::detail;
namespace test_util = xz::redis::test_util;

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};

    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    // cfg.connect_timeout = std::chrono::milliseconds{1000};
    // cfg.request_timeout = std::chrono::milliseconds{1000};
  }

  config cfg;
};

TEST_F(PipelineTest, ExecutePing) {
  io_context ctx;
  redis_detail::connection conn{ctx, cfg};

  auto f = [&]() -> awaitable<void> {
    co_await conn.run();

    request req;
    req.push("PING");

    response<std::string> resp;
    co_await conn.execute(req, resp);

    EXPECT_TRUE(std::get<0>(resp).has_value());
    EXPECT_EQ(std::get<0>(resp).value(), "PONG");
  };

  auto res = test_util::run_io(ctx, f, [&]() { conn.stop(); });
  ASSERT_TRUE(res.ec == std::error_code{} && res.what.empty()) << (res.what.empty() ? "unknown error" : res.what);
}

TEST_F(PipelineTest, TwoConcurrentExecutesAreSerialized) {
  io_context ctx;
  redis_detail::connection conn{ctx, cfg};

  response<std::string> pong;
  response<std::string> echo;

  auto t1 = [&]() -> awaitable<void> {
    request req;
    req.push("PING");
    co_await conn.execute(req, pong);
  };

  auto t2 = [&]() -> awaitable<void> {
    request req;
    req.push("ECHO", "hello");
    co_await conn.execute(req, echo);
  };

  auto f = [&]() -> awaitable<void> {
    co_await conn.run();
    // Start both "concurrently" from the caller's perspective; pipeline serializes them.
    co_await when_all(t1(), t2());
  };

  auto res = test_util::run_io(ctx, f, [&]() { conn.stop(); });

  ASSERT_TRUE(res.ec == std::error_code{} && res.what.empty()) << (res.what.empty() ? "unknown error" : res.what);

  EXPECT_TRUE(std::get<0>(pong).has_value());
  EXPECT_EQ(std::get<0>(pong).value(), "PONG");

  EXPECT_TRUE(std::get<0>(echo).has_value());
  EXPECT_EQ(std::get<0>(echo).value(), "hello");
}
