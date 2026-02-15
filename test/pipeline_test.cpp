#include <gtest/gtest.h>

#include <rediscoro/detail/pipeline.hpp>
#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/message.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

namespace {

class counting_sink final : public rediscoro::detail::response_sink {
 public:
  explicit counting_sink(std::size_t n) : expected_(n) {}

  [[nodiscard]] auto expected_replies() const noexcept -> std::size_t override { return expected_; }

  [[nodiscard]] auto is_complete() const noexcept -> bool override {
    return (msgs_ + errs_) == expected_;
  }

  [[nodiscard]] auto msg_count() const noexcept -> std::size_t { return msgs_; }
  [[nodiscard]] auto err_count() const noexcept -> std::size_t { return errs_; }

 protected:
  auto do_deliver(rediscoro::resp3::message) -> void override { msgs_ += 1; }
  auto do_deliver_error(rediscoro::error_info) -> void override { errs_ += 1; }

 private:
  std::size_t expected_{0};
  std::size_t msgs_{0};
  std::size_t errs_{0};
};

}  // namespace

TEST(pipeline_test, partial_write_and_next_write_buffer) {
  rediscoro::detail::pipeline p;
  auto sink = std::make_shared<counting_sink>(1);

  rediscoro::request req{"PING"};
  const auto wire = req.wire();
  ASSERT_FALSE(wire.empty());

  p.push(req, sink);
  ASSERT_TRUE(p.has_pending_write());

  auto b1 = p.next_write_buffer();
  EXPECT_EQ(b1.size(), wire.size());

  p.on_write_done(1);
  auto b2 = p.next_write_buffer();
  EXPECT_EQ(b2.size(), wire.size() - 1);

  p.on_write_done(wire.size() - 1);
  EXPECT_FALSE(p.has_pending_write());
  EXPECT_TRUE(p.has_pending_read());
}

TEST(pipeline_test, multi_reply_dispatch_completes_sink) {
  rediscoro::detail::pipeline p;

  rediscoro::request req;
  req.push("PING");
  req.push("PING");
  ASSERT_EQ(req.reply_count(), 2u);

  auto sink = std::make_shared<counting_sink>(2);
  p.push(req, sink);

  // Pretend socket wrote everything.
  const auto wire = req.wire();
  p.on_write_done(wire.size());
  ASSERT_TRUE(p.has_pending_read());

  p.on_message(rediscoro::resp3::message{rediscoro::resp3::simple_string{"OK"}});
  EXPECT_EQ(sink->msg_count(), 1u);
  EXPECT_FALSE(sink->is_complete());

  p.on_message(rediscoro::resp3::message{rediscoro::resp3::simple_string{"OK"}});
  EXPECT_EQ(sink->msg_count(), 2u);
  EXPECT_TRUE(sink->is_complete());
  EXPECT_FALSE(p.has_pending_read());
}

TEST(pipeline_test, clear_all_fills_errors_for_pending_and_awaiting) {
  rediscoro::detail::pipeline p;

  rediscoro::request req;
  req.push("PING");
  req.push("PING");
  auto sink = std::make_shared<counting_sink>(2);

  p.push(req, sink);

  // Before any write/read, clear_all should deliver 2 errors.
  p.clear_all(rediscoro::client_errc::connection_closed);
  EXPECT_TRUE(sink->is_complete());
  EXPECT_EQ(sink->err_count(), 2u);
  EXPECT_FALSE(p.has_pending_write());
  EXPECT_FALSE(p.has_pending_read());
}

TEST(pipeline_test, deadline_order_and_expiration) {
  rediscoro::detail::pipeline p;
  auto s1 = std::make_shared<counting_sink>(1);
  auto s2 = std::make_shared<counting_sink>(1);

  rediscoro::request req1{"PING"};
  rediscoro::request req2{"PING"};

  const auto d1 = rediscoro::detail::pipeline::clock::now() + std::chrono::milliseconds(200);
  const auto d2 = rediscoro::detail::pipeline::clock::now() + std::chrono::milliseconds(20);

  // FIFO write queue semantics: next_deadline() is based on the queue front.
  p.push(req1, s1, d1);
  p.push(req2, s2, d2);
  EXPECT_EQ(p.next_deadline(), d1);

  // After req1 is fully written, req2 is now the pending-write front.
  p.on_write_done(req1.wire().size());
  EXPECT_EQ(p.next_deadline(), d2);

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_TRUE(p.has_expired());
}

TEST(pipeline_test, clear_all_mixed_pending_and_awaiting) {
  rediscoro::detail::pipeline p;

  rediscoro::request req1{"PING"};
  rediscoro::request req2;
  req2.push("PING");
  req2.push("PING");

  auto s1 = std::make_shared<counting_sink>(1);
  auto s2 = std::make_shared<counting_sink>(2);

  p.push(req1, s1);
  p.push(req2, s2);

  // Move req1 to awaiting_read; req2 remains pending_write.
  p.on_write_done(req1.wire().size());
  ASSERT_TRUE(p.has_pending_read());
  ASSERT_TRUE(p.has_pending_write());

  p.clear_all(rediscoro::client_errc::connection_closed);

  EXPECT_TRUE(s1->is_complete());
  EXPECT_TRUE(s2->is_complete());
  EXPECT_EQ(s1->err_count(), 1u);
  EXPECT_EQ(s2->err_count(), 2u);
  EXPECT_FALSE(p.has_pending_write());
  EXPECT_FALSE(p.has_pending_read());
}
