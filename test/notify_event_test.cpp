#include <gtest/gtest.h>

#include <rediscoro/detail/notify_event.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <atomic>
#include <thread>

using rediscoro::detail::notify_event;

TEST(notify_event_test, notify_before_wait_consumes_counts) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);
  notify_event ev;
  int consumed = 0;

  auto task = [&]() -> iocoro::awaitable<void> {
    co_await ev.wait();
    consumed += 1;
    co_await ev.wait();
    consumed += 1;
    guard.reset();  // allow context to stop after completion
    co_return;
  };

  // Issue notifications before the waiter starts; both should be consumed.
  ev.notify();
  ev.notify();

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  EXPECT_EQ(consumed, 2);
}

TEST(notify_event_test, resume_on_original_executor) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);
  notify_event ev;

  std::thread::id executor_thread{};
  std::thread::id resumed_thread{};
  std::atomic_bool done{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    executor_thread = std::this_thread::get_id();
    co_await ev.wait();
    resumed_thread = std::this_thread::get_id();
    done.store(true, std::memory_order_release);
    guard.reset();  // allow context to stop after completion
    co_return;
  };

  std::thread notifier([&]() {
    std::this_thread::sleep_for(5ms);
    ev.notify();
  });

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();
  notifier.join();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_EQ(executor_thread, resumed_thread);
}


