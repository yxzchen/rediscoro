#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/when_all.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

using namespace xz::io;
using namespace xz::redis;

static void fail_and_stop_on_exception(std::exception_ptr eptr) {
  if (!eptr) return;
  try {
    std::rethrow_exception(eptr);
  } catch (std::exception const& e) {
    ADD_FAILURE() << "Unhandled exception in spawned coroutine: " << e.what();
  } catch (...) {
    ADD_FAILURE() << "Unhandled unknown exception in spawned coroutine";
  }
}

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
    // Exercise handshake steps against local Redis.
    cfg.database = 1;
    cfg.client_name = std::string{"redisxz-test"};

    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    // cfg.connect_timeout = std::chrono::milliseconds{1000};
    // cfg.request_timeout = std::chrono::milliseconds{1000};
  }

  config cfg;
};

TEST_F(PipelineTest, ExecutePing) {
  io_context ctx;

  co_spawn(
      ctx,
      [&]() -> awaitable<void> {
        connection conn{ctx, cfg};
        co_await conn.run();

        request req;
        req.push("PING");

        response<std::string> resp;
        co_await conn.execute(req, resp);

        EXPECT_TRUE(std::get<0>(resp).has_value());
        EXPECT_EQ(std::get<0>(resp).value(), "PONG");
      },
      [&](std::exception_ptr eptr) { fail_and_stop_on_exception(eptr); });

  ctx.run();
}

TEST_F(PipelineTest, TwoConcurrentExecutesAreSerialized) {
  io_context ctx;

  co_spawn(
      ctx,
      [&]() -> awaitable<void> {
        connection conn{ctx, cfg};
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

        co_await conn.run();
        // Start both "concurrently" from the caller's perspective; pipeline serializes them.
        co_await when_all(t1(), t2());

        EXPECT_TRUE(std::get<0>(pong).has_value());
        EXPECT_EQ(std::get<0>(pong).value(), "PONG");

        EXPECT_TRUE(std::get<0>(echo).has_value());
        EXPECT_EQ(std::get<0>(echo).value(), "hello");
      },
      [&](std::exception_ptr eptr) { fail_and_stop_on_exception(eptr); });

  ctx.run();
}
