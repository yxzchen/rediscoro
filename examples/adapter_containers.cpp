#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

auto adapter_containers_task() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.reconnection.enabled = false;

  rediscoro::client c{ex, cfg};

  auto cr = co_await c.connect();
  if (!cr) {
    std::cerr << "connect failed: " << cr.error().to_string() << "\n";
    co_return;
  }

  constexpr auto k_list_key = "rediscoro:example:list";
  constexpr auto k_hash_key = "rediscoro:example:hash";

  auto clear = co_await c.exec<std::int64_t>("DEL", k_list_key, k_hash_key);
  if (!clear.get<0>()) {
    std::cerr << "DEL failed: " << clear.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  auto rpush = co_await c.exec<std::int64_t>("RPUSH", k_list_key, "alpha", "beta", "gamma");
  if (!rpush.get<0>()) {
    std::cerr << "RPUSH failed: " << rpush.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  auto list_resp = co_await c.exec<std::vector<std::string>>("LRANGE", k_list_key, "0", "-1");
  if (!list_resp.get<0>()) {
    std::cerr << "LRANGE failed: " << list_resp.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  std::cout << "LRANGE -> vector<string>:\n";
  for (auto const& item : *list_resp.get<0>()) {
    std::cout << "  - " << item << "\n";
  }

  auto hset = co_await c.exec<std::int64_t>("HSET", k_hash_key, "name", "rediscoro", "lang", "cpp");
  if (!hset.get<0>()) {
    std::cerr << "HSET failed: " << hset.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  auto map_resp = co_await c.exec<std::map<std::string, std::string>>("HGETALL", k_hash_key);
  if (!map_resp.get<0>()) {
    std::cerr << "HGETALL failed: " << map_resp.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  std::cout << "HGETALL -> map<string, string>:\n";
  for (auto const& [k, v] : *map_resp.get<0>()) {
    std::cout << "  " << k << " = " << v << "\n";
  }

  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), adapter_containers_task(), iocoro::detached);
  ctx.run();
  return 0;
}
