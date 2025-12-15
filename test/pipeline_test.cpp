#include <xz/io/io_context.hpp>
#include <xz/io/when_all.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

using namespace xz::io;
using namespace xz::redis;
namespace redis_detail = xz::redis::detail;

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
  }

  config cfg;
};

TEST_F(PipelineTest, ExecutePing) {
  io_context ctx;
  redis_detail::connection conn{ctx, cfg};

  auto task = [&]() -> awaitable<void> {
    co_await conn.run();

    request req;
    req.push("PING");

    response<std::string> resp;
    co_await conn.execute(req, resp);

    EXPECT_TRUE(std::get<0>(resp).has_value());
    EXPECT_EQ(std::get<0>(resp).value(), "PONG");

    // Allow ctx.run() to return (read_loop keeps the io_context alive via fd registrations).
    conn.stop();
  };

  co_spawn(ctx, task(), use_detached);
  ctx.run();
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

  auto main_task = [&]() -> awaitable<void> {
    co_await conn.run();
    // Start both "concurrently" from the caller's perspective; pipeline serializes them.
    co_await when_all(t1(), t2());
    conn.stop();
  };

  co_spawn(ctx, main_task(), use_detached);
  ctx.run();

  EXPECT_TRUE(std::get<0>(pong).has_value());
  EXPECT_EQ(std::get<0>(pong).value(), "PONG");

  EXPECT_TRUE(std::get<0>(echo).has_value());
  EXPECT_EQ(std::get<0>(echo).value(), "hello");
}


