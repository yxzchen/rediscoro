#include <xz/redis/connection.hpp>
#include <xz/redis/detail/connection_impl.hpp>
#include <xz/redis/impl/connection_impl.ipp>

namespace xz::redis {

connection::connection(io::io_context& ctx, config cfg)
    : impl_{std::make_shared<detail::connection_impl>(ctx, std::move(cfg))} {}

connection::~connection() {
  if (impl_) {
    impl_->stop();
  }
}

auto connection::run() -> io::awaitable<void> {
  co_await impl_->run();
}

auto connection::execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void> {
  co_await impl_->execute_any(req, adapter);
}

void connection::stop() {
  if (impl_) {
    impl_->stop();
  }
}

auto connection::graceful_stop() -> io::awaitable<void> {
  if (impl_) {
    co_await impl_->graceful_stop();
  }
}

auto connection::current_state() const noexcept -> state {
  return impl_ ? static_cast<state>(impl_->current_state()) : state::stopped;
}

auto connection::is_running() const noexcept -> bool {
  return impl_ && impl_->is_running();
}

auto connection::error() const -> std::error_code {
  return impl_ ? impl_->error() : std::error_code{};
}

auto connection::get_executor() noexcept -> io::io_context& {
  return impl_->get_executor();
}

}  // namespace xz::redis
