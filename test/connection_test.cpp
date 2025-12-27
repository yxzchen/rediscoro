#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/connection.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/response.hpp>

#include <rediscoro/src.hpp>

#include <gtest/gtest.h>

#include "async_test_util.hpp"

#include <map>
#include <string>
#include <vector>

using namespace rediscoro;
using namespace iocoro;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
    cfg.auto_reconnect = false;  // Disable for most tests
    // Exercise handshake steps against local Redis.
    cfg.database = 1;
    cfg.client_name = std::string{"rediscoro-test"};
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
    EXPECT_EQ(name.value().value(), "rediscoro-test");
  });
}

TEST_F(ConnectionTest, ConnectTimeoutToHttpServer) {
  io_context ctx;
  config c = cfg;
  c.host = "153.3.238.127";
  c.port = 80;
  c.connect_timeout = std::chrono::milliseconds{1};
  c.request_timeout = std::chrono::milliseconds{200};

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, c};
    try {
      co_await conn.run();
      ADD_FAILURE() << "Expected connect timeout, but run() succeeded";
    } catch (std::system_error const& e) {
      EXPECT_EQ(e.code(), iocoro::error::timed_out);
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
    req.push("BLPOP", "rediscoro-test-nonexistent-key", "1");
    response0<ignore_t> resp;

    try {
      co_await conn.execute(req, resp);
      ADD_FAILURE() << "Expected command timeout, but execute succeeded";
    } catch (std::system_error const& e) {
      EXPECT_EQ(e.code(), iocoro::error::timed_out);
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
    std::string const key_counter = "rediscoro-test:counter";
    std::string const key_hash = "rediscoro-test:hash";
    std::string const key_list = "rediscoro-test:list";

    // DEL keys (best-effort).
    {
      request del;
      del.push("DEL", key_counter, key_hash, key_list);
      response0<int> del_resp;
      co_await conn.execute(del, del_resp);
      (void)del_resp;
    }

    // INCR -> int
    {
      request r;
      r.push("INCR", key_counter);
      response0<int> out;
      co_await conn.execute(r, out);
      if (!out.has_value()) {
        ADD_FAILURE() << "INCR failed: " << out.error().message;
        co_return;
      }
      EXPECT_GE(out.value(), 1);
    }

    // ECHO -> string
    {
      request r;
      r.push("ECHO", "hello");
      response0<std::string> out;
      co_await conn.execute(r, out);
      if (!out.has_value()) {
        ADD_FAILURE() << "ECHO failed: " << out.error().message;
        co_return;
      }
      EXPECT_EQ(out.value(), "hello");
    }

    // GET missing -> optional<string> == null
    {
      request r;
      r.push("GET", "rediscoro-test:missing-key");
      response0<std::optional<std::string>> out;
      co_await conn.execute(r, out);
      if (!out.has_value()) {
        ADD_FAILURE() << "GET failed: " << out.error().message;
        co_return;
      }
      EXPECT_FALSE(out.value().has_value());
    }

    // HSET + HGETALL -> map<string,string> (RESP3 map)
    {
      request r1;
      r1.push("HSET", key_hash, "field", "value");
      response0<int> hset;
      co_await conn.execute(r1, hset);
      if (!hset.has_value()) {
        ADD_FAILURE() << "HSET failed: " << hset.error().message;
        co_return;
      }

      request r2;
      r2.push("HGETALL", key_hash);
      response0<std::map<std::string, std::string>> hgetall;
      co_await conn.execute(r2, hgetall);
      if (!hgetall.has_value()) {
        ADD_FAILURE() << "HGETALL failed: " << hgetall.error().message;
        co_return;
      }
      EXPECT_EQ(hgetall.value().at("field"), "value");
    }

    // RPUSH + LRANGE -> vector<string>
    {
      request r1;
      r1.push("RPUSH", key_list, "a", "b", "c");
      response0<int> rpush;
      co_await conn.execute(r1, rpush);
      if (!rpush.has_value()) {
        ADD_FAILURE() << "RPUSH failed: " << rpush.error().message;
        co_return;
      }

      request r2;
      r2.push("LRANGE", key_list, "0", "-1");
      response0<std::vector<std::string>> lrange;
      co_await conn.execute(r2, lrange);
      if (!lrange.has_value()) {
        ADD_FAILURE() << "LRANGE failed: " << lrange.error().message;
        co_return;
      }
      auto const& v = lrange.value();
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
      response0<ignore_t> out;
      co_await conn.execute(r, out);
      EXPECT_FALSE(out.has_value());
      EXPECT_FALSE(out.error().message.empty());
    }

    // Type mismatch: ECHO returns string, parse as int => error in result.
    {
      request r;
      r.push("ECHO", "not-a-number");
      response0<int> out;
      co_await conn.execute(r, out);
      EXPECT_FALSE(out.has_value());
      EXPECT_FALSE(out.error().message.empty());
    }
  });
}

TEST_F(ConnectionTest, MultiCommandSingleRequestAllOk) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    // One request containing multiple commands (pipelined).
    request req;
    req.push("PING");
    req.push("ECHO", "hello");
    req.push("INCR", "rediscoro-test:multi:counter");

    dynamic_response<ignore_t> resp;
    co_await conn.execute(req, resp);

    EXPECT_EQ(resp.size(), req.expected_responses());
    for (std::size_t i = 0; i < resp.size(); ++i) {
      EXPECT_TRUE(resp[i].has_value()) << "reply[" << i << "] error: " << resp[i].error().message;
    }
  });
}

TEST_F(ConnectionTest, MultiCommandSingleRequestSurfacesPerReplyError) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    request req;
    req.push("PING");
    req.push("THIS_COMMAND_DOES_NOT_EXIST");
    req.push("PING");

    dynamic_response<ignore_t> resp;
    co_await conn.execute(req, resp);

    if (resp.size() != req.expected_responses()) {
      ADD_FAILURE() << "Expected " << req.expected_responses() << " replies, got " << resp.size();
      co_return;
    }
    EXPECT_TRUE(resp[0].has_value());
    EXPECT_FALSE(resp[1].has_value());
    EXPECT_TRUE(resp[2].has_value());
  });
}

TEST_F(ConnectionTest, MultiCommandSingleRequestGenericResponsePreservesBoundaries) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    request req;
    req.push("PING");
    req.push("ECHO", "hello");

    generic_response resp;
    co_await conn.execute(req, resp);

    if (!resp.has_value()) {
      ADD_FAILURE() << "Execute failed: " << resp.error().message;
      co_return;
    }
    EXPECT_EQ(resp.value().size(), req.expected_responses());
    for (auto const& msg : resp.value()) {
      EXPECT_FALSE(msg.empty());
    }
  });
}

TEST_F(ConnectionTest, TupleResponseIntStringVectorStringWorks) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    std::string const key_counter = "rediscoro-test:multi:tuple:counter";
    std::string const key_list = "rediscoro-test:multi:tuple:list";

    // Best-effort cleanup.
    {
      request del;
      del.push("DEL", key_counter, key_list);
      response0<int> del_resp;
      co_await conn.execute(del, del_resp);
      (void)del_resp;
    }

    // Seed list for LRANGE.
    {
      request seed;
      seed.push("RPUSH", key_list, "a", "b", "c");
      response0<int> rpush;
      co_await conn.execute(seed, rpush);
      if (!rpush.has_value()) {
        ADD_FAILURE() << "RPUSH failed: " << rpush.error().message;
        co_return;
      }
    }

    // One request, three different typed replies.
    request req;
    req.push("INCR", key_counter);          // int
    req.push("ECHO", "hello");              // string
    req.push("LRANGE", key_list, "0", "-1"); // vector<string>

    response<int, std::string, std::vector<std::string>> resp;
    co_await conn.execute(req, resp);

    auto& r0 = std::get<0>(resp);
    auto& r1 = std::get<1>(resp);
    auto& r2 = std::get<2>(resp);

    if (!r0.has_value()) {
      ADD_FAILURE() << "INCR failed: " << r0.error().message;
      co_return;
    }
    if (!r1.has_value()) {
      ADD_FAILURE() << "ECHO failed: " << r1.error().message;
      co_return;
    }
    if (!r2.has_value()) {
      ADD_FAILURE() << "LRANGE failed: " << r2.error().message;
      co_return;
    }

    EXPECT_GE(r0.value(), 1);
    EXPECT_EQ(r1.value(), "hello");
    EXPECT_EQ(r2.value(), (std::vector<std::string>{"a", "b", "c"}));
  });
}

TEST_F(ConnectionTest, ExecuteOneSingleTypeWorks) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    std::string const key_counter = "rediscoro-test:execute_one:counter";

    request req;
    req.push("INCR", key_counter);

    auto out = co_await conn.execute_one<int>(req);
    if (!out.has_value()) {
      ADD_FAILURE() << "INCR failed: " << out.error().message;
      co_return;
    }
    EXPECT_GE(out.value(), 1);
  });
}

TEST_F(ConnectionTest, ExecuteOneMultipleTypesWorkWithGenericContainers) {
  io_context ctx;

  test_util::run_async(ctx, [&]() -> awaitable<void> {
    connection conn{ctx, cfg};
    co_await conn.run();

    std::string const key_list = "rediscoro-test:execute_one:list";

    // Seed list for LRANGE.
    {
      request seed;
      seed.push("DEL", key_list);
      seed.push("RPUSH", key_list, "a", "b", "c");
      dynamic_response<ignore_t> seed_resp;
      co_await conn.execute(seed, seed_resp);
    }

    request req;
    req.push("ECHO", "hello");
    req.push("LRANGE", key_list, "0", "-1");
    req.push("GET", "rediscoro-test:execute_one:missing");

    auto resp = co_await conn.execute_one<std::string, std::vector<std::string>, std::optional<std::string>>(req);
    auto& r0 = std::get<0>(resp);
    auto& r1 = std::get<1>(resp);
    auto& r2 = std::get<2>(resp);

    if (!r0.has_value()) {
      ADD_FAILURE() << "ECHO failed: " << r0.error().message;
      co_return;
    }
    if (!r1.has_value()) {
      ADD_FAILURE() << "LRANGE failed: " << r1.error().message;
      co_return;
    }
    if (!r2.has_value()) {
      ADD_FAILURE() << "GET failed: " << r2.error().message;
      co_return;
    }

    EXPECT_EQ(r0.value(), "hello");
    EXPECT_EQ(r1.value(), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_FALSE(r2.value().has_value());
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
