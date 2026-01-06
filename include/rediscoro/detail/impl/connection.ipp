#pragma once

#include <rediscoro/detail/connection.hpp>
#include <iocoro/bind_executor.hpp>

namespace rediscoro::detail {

inline connection::connection(iocoro::io_executor ex, config cfg)
  : cfg_(std::move(cfg))
  , executor_(ex)
  , socket_(executor_.get_io_executor()) {
}

inline auto connection::run_worker() -> void {
  // TODO: Implementation
  // - Spawn worker_loop on the connection's strand
  // - Use use_awaitable completion token
  // - Save the awaitable in worker_awaitable_ for close() to use
}

inline auto connection::connect() -> iocoro::awaitable<std::error_code> {
  // TODO: Implementation
  //
  // CRITICAL: All state mutations MUST occur on the connection's strand to prevent
  // data races with worker_loop.
  //
  // 1. Handle CLOSED state - support retry (can do before strand switch)
  //    if (state_ == CLOSED) {
  //      // Previous connection failed and was cleaned up
  //      // Reset state to allow retry
  //      state_ = INIT;
  //      last_error_.reset();
  //      reconnect_count_ = 0;
  //      worker_awaitable_.reset();  // Previous worker has exited
  //    }
  //
  // 2. Check if already connected (idempotency)
  //    if (state_ == OPEN) co_return std::error_code{};
  //
  // 3. Check if currently connecting (concurrent call not supported)
  //    if (state_ == CONNECTING) {
  //      co_return make_error_code(error::concurrent_operation);
  //    }
  //
  // 4. Start worker_loop if not already started
  //    if (!worker_awaitable_.has_value()) {
  //      run_worker();  // Spawns worker_loop, sets worker_awaitable_
  //    }
  //
  // 5. Switch to connection's strand
  //    co_await switch_to(executor_.strand());
  //
  // 6. Check cancellation (handle connect() + close() race)
  //    if (cancel_.is_cancelled()) {
  //      co_return make_error_code(error::operation_aborted);
  //    }
  //
  // 7. Execute initial connection
  //    state_ = CONNECTING;
  //    co_await do_connect();
  //
  // 8. Check cancellation again (do_connect may take time)
  //    if (cancel_.is_cancelled()) {
  //      // close() was called during connection
  //      // Fall through to cleanup (step 10)
  //    }
  //
  // 9. Check result
  //    if (state_ == OPEN) {
  //      co_return std::error_code{};  // Success
  //    }
  //
  // 10. Handle failure - CRITICAL: Clean up all resources and wait for worker exit
  //     // - Set cancel flag to signal worker_loop to exit
  //     // - Close socket (if still open)
  //     // - Clear all pending requests (pipeline.clear_all)
  //     // - Notify worker to wake up and exit (wakeup_.notify)
  //     // - Wait for worker_loop to complete (co_await *worker_awaitable_)
  //     // - Clear worker_awaitable_ to allow restart on retry
  //     // - Transition to CLOSED
  //     // - Return error (use operation_aborted if cancelled, otherwise last_error_)
  //
  // IMPORTANT: On failure, this method waits for all background coroutines to exit
  // before returning, ensuring clean resource cleanup. This allows retry by calling
  // connect() again.
  co_return std::error_code{};
}

inline auto connection::close() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // 1. Check if already closed (idempotency)
  //    if (state_ == CLOSED) co_return;
  //
  // 2. Check if worker is running
  //    if (!worker_awaitable_.has_value()) {
  //      // Worker never started or already cleaned up
  //      // Just transition to CLOSED
  //      state_ = CLOSED;
  //      co_return;
  //    }
  //
  // 3. Request cancellation
  //    cancel_.request_cancel();
  //
  // 4. Notify worker to wake up and exit
  //    wakeup_.notify();
  //
  // 5. Wait for worker_loop to complete
  //    co_await *worker_awaitable_;
  //
  // 6. Verify post-condition
  //    REDISCORO_ASSERT(state_ == CLOSED);
  //
  // IMPORTANT: This method waits for worker_loop to completely exit before returning.
  // After this method returns, it's safe to destroy the connection object.
  co_return;
}

inline auto connection::enqueue_impl(request req, response_sink* sink) -> void {
  // TODO: Implementation
  //
  // 1. Check current state and reject early if not ready
  //    switch (state_) {
  //      case INIT:
  //      case CONNECTING:
  //        // Connection not established yet
  //        sink->deliver_error(make_error_code(error::not_connected));
  //        return;
  //
  //      case FAILED:
  //        // Connection lost due to runtime error (may be reconnecting in background)
  //        sink->deliver_error(make_error_code(error::connection_lost));
  //        return;
  //
  //      case CLOSING:
  //      case CLOSED:
  //        // Connection shut down
  //        sink->deliver_error(make_error_code(error::connection_closed));
  //        return;
  //
  //      case OPEN:
  //      case RECONNECTING:
  //        // Accept request
  //        break;
  //    }
  //
  // 2. Add request to pipeline
  //    pipeline_.push(std::move(req), sink);
  //
  // 3. Notify worker loop to process the request
  //    wakeup_.notify();
}

inline auto connection::worker_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // Design philosophy: This loop is a pure "runtime request processor".
  // It does NOT handle initial connection/handshake - that's connect()'s job.
  //
  // Main loop structure:
  // while (!cancel_.is_cancelled() && state_ != CLOSED) {
  //   co_await wakeup_.wait();
  //
  //   if (cancel_.is_cancelled()) break;
  //
  //   // State dispatch
  //   if (state_ == OPEN) {
  //     // Normal operation: drain all pending work
  //     while (pipeline_.has_pending_write() || pipeline_.has_pending_read()) {
  //       if (pipeline_.has_pending_write()) {
  //         co_await do_write();
  //         if (state_ == FAILED) break;  // Error during write
  //       }
  //       if (pipeline_.has_pending_read()) {
  //         co_await do_read();
  //         if (state_ == FAILED) break;  // Error during read
  //       }
  //     }
  //   }
  //   else if (state_ == FAILED) {
  //     // Runtime error - attempt reconnection or close
  //     // NOTE: This is ONLY for runtime errors after reaching OPEN
  //     // Initial connection failure is handled by connect() directly
  //     if (cfg_.reconnection.enabled) {
  //       co_await do_reconnect();  // Attempts reconnection, may transition to OPEN or CLOSED
  //     } else {
  //       break;  // Exit, will transition to CLOSED in cleanup
  //     }
  //   }
  //   // INIT/CONNECTING/RECONNECTING: do nothing, connect() owns the socket
  // }
  //
  // Cleanup:
  // - transition_to_closed()
  //
  // KEY INSIGHT: By not handling initial connection here, we eliminate:
  // - Handshake/request interleaving logic
  // - CONNECTING state special cases
  // - Pipeline phase distinction (handshake vs normal)
  // - Worker/connect coordination complexity
  co_return;
}

inline auto connection::do_connect() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // This method performs TCP connection + RESP3 handshake.
  // On error, it sets state_ = FAILED and last_error_ with appropriate error code.
  //
  // 1. Resolve address
  //    - On DNS failure: set last_error_ = timeout or system error
  //
  // 2. TCP connect with timeout
  //    - co_await async_connect(socket_, endpoint, timeout)
  //    - On timeout: set last_error_ = error::timeout, state_ = FAILED, co_return
  //    - On error: set last_error_ = system error, state_ = FAILED, co_return
  //
  // 3. Check cancel (in case close() was called during connect)
  //    - if (cancel_.is_cancelled()) { state_ = FAILED; co_return; }
  //
  // 4. Send RESP3 handshake commands (directly via socket, NOT via pipeline)
  //    - HELLO 3
  //    - AUTH (if username/password configured)
  //    - SELECT (if database != 0)
  //    - CLIENT SETNAME (if client_name configured)
  //
  //    For each command:
  //    - co_await async_write(socket_, encode_command(...))
  //    - co_await async_read + parse response
  //    - If response is error: set last_error_ = error::handshake_failed, state_ = FAILED, co_return
  //    - If timeout: set last_error_ = error::timeout, state_ = FAILED, co_return
  //
  // 5. Handshake complete
  //    - state_ = OPEN
  //    - reconnect_count_ = 0 (reset on success)
  //    - co_return
  //
  // NOTE: This method does NOT use pipeline during handshake.
  // Pipeline is only for user requests after reaching OPEN state.
  co_return;
}

inline auto connection::do_read() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - async_read_some into buffer
  // - Feed data to parser
  // - Parse messages and dispatch to pipeline
  co_return;
}

inline auto connection::do_write() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Get next_write_buffer from pipeline
  // - async_write to socket
  // - Notify pipeline of bytes written
  co_return;
}

inline auto connection::handle_error(std::error_code ec) -> void {
  // TODO: Implementation
  // - Log error
  // - Transition to FAILED state
  // - Clear pipeline with error
}

inline auto connection::transition_to_closed() -> void {
  // TODO: Implementation
  // - Close socket
  // - Clear pipeline
  // - Set state to CLOSED
}

template <typename... Ts>
auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();
  enqueue_impl(std::move(req), slot.get());
  return slot;
}

template <typename T>
auto connection::enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());
  enqueue_impl(std::move(req), slot.get());
  return slot;
}

}  // namespace rediscoro::detail
