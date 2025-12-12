#pragma once

#include <chrono>
#include <deque>
#include <optional>

namespace xz::redis {

struct pending_request {
  std::size_t expected_responses;
  std::chrono::steady_clock::time_point deadline;
};

class pipeline {
 public:
  pipeline() = default;

  void push(std::size_t expected_responses, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    requests_.push_back({expected_responses, deadline});
  }

  void pop() {
    if (!requests_.empty()) {
      requests_.pop_front();
    }
  }

  auto front() const -> pending_request const* {
    if (requests_.empty()) {
      return nullptr;
    }
    return &requests_.front();
  }

  auto empty() const noexcept -> bool { return requests_.empty(); }

  auto size() const noexcept -> std::size_t { return requests_.size(); }

  void clear() noexcept { requests_.clear(); }

  auto find_timed_out(std::chrono::steady_clock::time_point now) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < requests_.size(); ++i) {
      if (requests_[i].deadline <= now) {
        return i;
      }
    }
    return std::nullopt;
  }

  auto next_deadline() const -> std::optional<std::chrono::steady_clock::time_point> {
    if (requests_.empty()) {
      return std::nullopt;
    }
    return requests_.front().deadline;
  }

 private:
  std::deque<pending_request> requests_;
};

}  // namespace xz::redis
