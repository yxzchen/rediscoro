#include <gtest/gtest.h>

#include <rediscoro/detail/pipeline.hpp>
#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <memory>

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
  auto do_deliver_error(rediscoro::error) -> void override { errs_ += 1; }

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
  p.clear_all(rediscoro::error::connection_closed);
  EXPECT_TRUE(sink->is_complete());
  EXPECT_EQ(sink->err_count(), 2u);
  EXPECT_FALSE(p.has_pending_write());
  EXPECT_FALSE(p.has_pending_read());
}
