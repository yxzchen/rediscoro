#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

struct trace_printer {
  std::size_t starts{0};
  std::size_t finishes{0};

  static auto on_start(void* user_data, rediscoro::request_trace_start const& ev) -> void {
    auto* self = static_cast<trace_printer*>(user_data);
    self->starts += 1;
    std::cout << "[trace start] id=" << ev.info.id << " kind=" << rediscoro::to_string(ev.info.kind)
              << " commands=" << ev.info.command_count << " wire_bytes=" << ev.info.wire_bytes
              << "\n";
  }

  static auto on_finish(void* user_data, rediscoro::request_trace_finish const& ev) -> void {
    auto* self = static_cast<trace_printer*>(user_data);
    self->finishes += 1;
    std::cout << "[trace finish] id=" << ev.info.id << " duration_ns=" << ev.duration.count()
              << " ok_count=" << ev.ok_count << " error_count=" << ev.error_count;
    if (ev.primary_error) {
      std::cout << " primary_error=" << ev.primary_error.value()
                << " detail=" << ev.primary_error_detail;
    }
    std::cout << "\n";
  }
};

auto tracing_hooks_task() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  trace_printer printer{};
  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.reconnection.enabled = false;
  cfg.trace_hooks = {
    .user_data = &printer,
    .on_start = &trace_printer::on_start,
    .on_finish = &trace_printer::on_finish,
  };

  rediscoro::client c{ex, cfg};

  auto cr = co_await c.connect();
  if (!cr) {
    std::cerr << "connect failed: " << cr.error().to_string() << "\n";
    co_return;
  }

  auto ping = co_await c.exec<std::string>("PING");
  if (!ping.get<0>()) {
    std::cerr << "PING failed: " << ping.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  constexpr auto k_key = "rediscoro:example:trace";
  auto set = co_await c.exec<std::string>("SET", k_key, "x");
  if (!set.get<0>()) {
    std::cerr << "SET failed: " << set.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  auto incr = co_await c.exec<std::int64_t>("INCR", k_key);
  if (!incr.get<0>()) {
    std::cout << "INCR failed as expected: " << incr.get<0>().error().to_string() << "\n";
  }

  co_await c.close();
  std::cout << "trace summary: starts=" << printer.starts << " finishes=" << printer.finishes
            << "\n";
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), tracing_hooks_task(), iocoro::detached);
  ctx.run();
  return 0;
}
