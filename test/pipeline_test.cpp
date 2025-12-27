#include <xz/io/io_context.hpp>
#include <xz/io/when_all.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/connection.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/response.hpp>

#include <gtest/gtest.h>

#include "async_test_util.hpp"

using namespace xz::io;
using namespace rediscoro;

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
    // Exercise handshake steps against local Redis.
    cfg.database = 1;
    cfg.client_name = std::string{"rediscoro-test"};

    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    // cfg.connect_timeout = std::chrono::milliseconds{1000};
    // cfg.request_timeout = std::chrono::milliseconds{1000};
  }

  config cfg;
};

TEST_F(PipelineTest, ExecutePing) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    request req;
    req.push("PING");

    response0<std::string> resp;
    co_await conn.execute(req, resp);

    EXPECT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value(), "PONG");
  });
}

TEST_F(PipelineTest, TwoConcurrentExecutesAreSerialized) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    response0<std::string> pong;
    response0<std::string> echo;

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

    co_await conn.run();
    // Start both "concurrently" from the caller's perspective; pipeline serializes them.
    co_await when_all(t1(), t2());

    EXPECT_TRUE(pong.has_value());
    EXPECT_EQ(pong.value(), "PONG");

    EXPECT_TRUE(echo.has_value());
    EXPECT_EQ(echo.value(), "hello");
  });
}
