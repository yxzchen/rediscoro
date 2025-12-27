#pragma once

#include <rediscoro/adapter/result.hpp>
#include <rediscoro/resp3/node.hpp>

#include <vector>

namespace rediscoro {

template <class... Ts>
using response = std::tuple<adapter::result<Ts>...>;

template <class T>
using response0 = adapter::result<T>;

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

}  // namespace rediscoro
