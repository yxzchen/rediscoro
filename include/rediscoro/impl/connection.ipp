#include <rediscoro/connection.hpp>
#include <rediscoro/detail/connection_impl.hpp>
#include <rediscoro/impl/connection_impl.ipp>

namespace rediscoro {

connection::connection(iocoro::io_executor ex, config cfg)
    : impl_{std::make_shared<detail::connection_impl>(ex, std::move(cfg))} {}

connection::~connection() {
  impl_->stop();
}

auto connection::run() -> iocoro::awaitable<void> {
  co_await impl_->run();
}

void connection::stop() {
  impl_->stop();
}

auto connection::graceful_stop() -> iocoro::awaitable<void> {
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

auto connection::get_executor() noexcept -> iocoro::io_executor {
  return impl_->get_executor();
}

}  // namespace rediscoro
