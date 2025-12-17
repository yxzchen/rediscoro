#pragma once

#include <xz/redis/adapter/result.hpp>
#include <xz/redis/resp3/node.hpp>

#include <vector>

namespace xz::redis {

template <class... Ts>
using response = std::tuple<adapter::result<Ts>...>;

/// A "generic" response that preserves message boundaries:
/// one owning `resp3::message` per received reply.
using generic_response = adapter::result<std::vector<resp3::msg>>;

/// A runtime-sized response: one `adapter::result<T>` per received reply message.
///
/// Intended for pipelining when the number of replies is only known at runtime, but the
/// per-reply type is uniform (`T`).
///
/// - Starts empty
/// - On each incoming reply message, a new element is appended and parsed into
/// - After `connection::execute(req, resp)`, `resp.size()` should equal `req.expected_responses()`
template <class T>
using dynamic_response = std::vector<adapter::result<T>>;

}  // namespace xz::redis
