#include <xz/redis/connection.hpp>
#include <xz/redis/detail/connection_impl.hpp>
#include <xz/redis/impl/connection_impl.ipp>

namespace xz::redis {

connection::connection(io::io_context& ctx, config cfg)
    : impl_{std::make_shared<detail::connection_impl>(ctx, std::move(cfg))} {}

connection::~connection() {
  impl_->stop();
}

auto connection::run() -> io::awaitable<void> {
  co_await impl_->run();
}

void connection::stop() {
  impl_->stop();
}

auto connection::graceful_stop() -> io::awaitable<void> {
  co_await impl_->graceful_stop();
}

auto connection::current_state() const noexcept -> state {
  return impl_->current_state();
}

auto connection::is_running() const noexcept -> bool {
  return impl_->is_running();
}

auto connection::error() const -> std::error_code {
  return impl_->error();
}

auto connection::get_executor() noexcept -> io::io_context& {
  return impl_->get_executor();
}

}  // namespace xz::redis
