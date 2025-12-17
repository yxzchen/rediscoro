#include <xz/io/io_context.hpp>
#include <xz/io/error.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

#include "async_test_util.hpp"

#include <map>
#include <string>
#include <vector>

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
    response0<std::optional<std::string>> name;
    co_await conn.execute(req, name);
    if (!name.has_value()) {
      ADD_FAILURE() << "CLIENT GETNAME failed: " << name.error().message;
      co_return;
    }
    if (!name.value().has_value()) {
      ADD_FAILURE() << "CLIENT GETNAME returned null (name not set)";
      co_return;
    }
    EXPECT_EQ(name.value().value(), "redisxz-test");
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

TEST_F(ConnectionTest, ExecuteVariousTypes) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    // Use DB=cfg.database (handshake already SELECTed it); keep keys under a test prefix.
    std::string const key_counter = "redisxz-test:counter";
    std::string const key_hash = "redisxz-test:hash";
    std::string const key_list = "redisxz-test:list";

    // DEL keys (best-effort).
    {
      request del;
      del.push("DEL", key_counter, key_hash, key_list);
      response<int> del_resp;
      co_await conn.execute(del, del_resp);
      (void)del_resp;
    }

    // INCR -> int
    {
      request r;
      r.push("INCR", key_counter);
      response<int> out;
      co_await conn.execute(r, out);
      if (!std::get<0>(out).has_value()) {
        ADD_FAILURE() << "INCR failed: " << std::get<0>(out).error().message;
        co_return;
      }
      EXPECT_GE(std::get<0>(out).value(), 1);
    }

    // ECHO -> string
    {
      request r;
      r.push("ECHO", "hello");
      response<std::string> out;
      co_await conn.execute(r, out);
      if (!std::get<0>(out).has_value()) {
        ADD_FAILURE() << "ECHO failed: " << std::get<0>(out).error().message;
        co_return;
      }
      EXPECT_EQ(std::get<0>(out).value(), "hello");
    }

    // GET missing -> optional<string> == null
    {
      request r;
      r.push("GET", "redisxz-test:missing-key");
      response<std::optional<std::string>> out;
      co_await conn.execute(r, out);
      if (!std::get<0>(out).has_value()) {
        ADD_FAILURE() << "GET failed: " << std::get<0>(out).error().message;
        co_return;
      }
      EXPECT_FALSE(std::get<0>(out).value().has_value());
    }

    // HSET + HGETALL -> map<string,string> (RESP3 map)
    {
      request r1;
      r1.push("HSET", key_hash, "field", "value");
      response<int> hset;
      co_await conn.execute(r1, hset);
      if (!std::get<0>(hset).has_value()) {
        ADD_FAILURE() << "HSET failed: " << std::get<0>(hset).error().message;
        co_return;
      }

      request r2;
      r2.push("HGETALL", key_hash);
      response<std::map<std::string, std::string>> hgetall;
      co_await conn.execute(r2, hgetall);
      if (!std::get<0>(hgetall).has_value()) {
        ADD_FAILURE() << "HGETALL failed: " << std::get<0>(hgetall).error().message;
        co_return;
      }
      EXPECT_EQ(std::get<0>(hgetall).value().at("field"), "value");
    }

    // RPUSH + LRANGE -> vector<string>
    {
      request r1;
      r1.push("RPUSH", key_list, "a", "b", "c");
      response<int> rpush;
      co_await conn.execute(r1, rpush);
      if (!std::get<0>(rpush).has_value()) {
        ADD_FAILURE() << "RPUSH failed: " << std::get<0>(rpush).error().message;
        co_return;
      }

      request r2;
      r2.push("LRANGE", key_list, "0", "-1");
      response<std::vector<std::string>> lrange;
      co_await conn.execute(r2, lrange);
      if (!std::get<0>(lrange).has_value()) {
        ADD_FAILURE() << "LRANGE failed: " << std::get<0>(lrange).error().message;
        co_return;
      }
      auto const& v = std::get<0>(lrange).value();
      if (v.size() != 3u) {
        ADD_FAILURE() << "LRANGE returned unexpected size: " << v.size();
        co_return;
      }
      EXPECT_EQ(v[0], "a");
      EXPECT_EQ(v[1], "b");
      EXPECT_EQ(v[2], "c");
    }
  });
}

TEST_F(ConnectionTest, ServerErrorAndTypeMismatchAreCapturedInResult) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    // Unknown command -> error captured in ignore_t result (execute does not throw).
    {
      request r;
      r.push("THIS_COMMAND_DOES_NOT_EXIST");
      response<ignore_t> out;
      co_await conn.execute(r, out);
      EXPECT_FALSE(std::get<0>(out).has_value());
      EXPECT_FALSE(std::get<0>(out).error().message.empty());
    }

    // Type mismatch: ECHO returns string, parse as int => error in result.
    {
      request r;
      r.push("ECHO", "not-a-number");
      response<int> out;
      co_await conn.execute(r, out);
      EXPECT_FALSE(std::get<0>(out).has_value());
      EXPECT_FALSE(std::get<0>(out).error().message.empty());
    }
  });
}

TEST_F(ConnectionTest, HandshakeFailsWithInvalidDatabase) {
  io_context ctx;
  config c = cfg;
  c.database = 9999;  // out of range on default Redis

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, c};
    try {
      co_await conn.run();
      ADD_FAILURE() << "Expected SELECT failure for invalid DB, but run() succeeded";
    } catch (std::system_error const&) {
      // Expected: handshake SELECT should fail.
    } catch (...) {
      ADD_FAILURE() << "Expected std::system_error for invalid DB";
    }
  });
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
