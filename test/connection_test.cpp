#include <xz/io/io_context.hpp>
#include <xz/io/error.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

#include "async_test_util.hpp"

using namespace xz::redis;
using namespace xz::io;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
    // Exercise handshake steps against local Redis.
    cfg.database = 1;
    cfg.client_name = std::string{"redisxz-test"};
  }

  config cfg;
};

TEST_F(ConnectionTest, RunBasic) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();
    EXPECT_TRUE(conn.is_running());

    // Verify handshake applied client name.
    request req;
    req.push("CLIENT", "GETNAME");
    response<std::optional<std::string>> name;
    co_await conn.execute(req, name);
    if (!std::get<0>(name).has_value()) {
      ADD_FAILURE() << "CLIENT GETNAME failed: " << std::get<0>(name).error().msg;
      co_return;
    }
    if (!std::get<0>(name).value().has_value()) {
      ADD_FAILURE() << "CLIENT GETNAME returned null (name not set)";
      co_return;
    }
    EXPECT_EQ(std::get<0>(name).value().value(), "redisxz-test");
  });
}

TEST_F(ConnectionTest, ConnectTimeoutToHttpServer) {
  io_context ctx;
  config c = cfg;
  c.host = "153.3.238.127";
  c.port = 80;
  c.connect_timeout = std::chrono::milliseconds{5};     // RTT ~30ms => should timeout
  c.request_timeout = std::chrono::milliseconds{200};

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, c};
    try {
      co_await conn.run();
      ADD_FAILURE() << "Expected connect timeout, but run() succeeded";
    } catch (std::system_error const& e) {
      EXPECT_EQ(e.code(), xz::io::error::timeout);
    } catch (...) {
      ADD_FAILURE() << "Expected std::system_error for connect timeout";
    }
  });
}

TEST_F(ConnectionTest, HandshakeFailsAgainstHttpServer) {
  io_context ctx;
  config c = cfg;
  c.host = "153.3.238.127";
  c.port = 80;
  c.connect_timeout = std::chrono::milliseconds{1000};
  c.request_timeout = std::chrono::milliseconds{500};

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, c};
    try {
      co_await conn.run();
      ADD_FAILURE() << "Expected handshake/protocol failure, but run() succeeded";
    } catch (std::system_error const&) {
      // Expected: HTTP response should break RESP parsing / handshake.
      EXPECT_TRUE(conn.current_state() == connection::state::failed ||
                  conn.current_state() == connection::state::stopped);
    } catch (...) {
      ADD_FAILURE() << "Expected std::system_error for handshake/protocol failure";
    }
  });
}

TEST_F(ConnectionTest, CommandTimeoutOnBlockingCommand) {
  io_context ctx;
  config c = cfg;
  c.host = "127.0.0.1";
  c.port = 6379;
  c.connect_timeout = std::chrono::milliseconds{1000};
  c.request_timeout = std::chrono::milliseconds{50};

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, c};
    co_await conn.run();

    // BLPOP blocks up to 1s; with request_timeout=50ms this should timeout.
    request req;
    req.push("BLPOP", "redisxz-test-nonexistent-key", "1");
    response<ignore_t> resp;

    try {
      co_await conn.execute(req, resp);
      ADD_FAILURE() << "Expected command timeout, but execute succeeded";
    } catch (std::system_error const& e) {
      EXPECT_EQ(e.code(), xz::io::error::timeout);
    } catch (...) {
      ADD_FAILURE() << "Expected std::system_error for command timeout";
    }
  });
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
