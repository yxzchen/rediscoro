#pragma once

#include <iocoro/expected.hpp>

namespace rediscoro {

using iocoro::expected;
using iocoro::unexpect;
using iocoro::unexpect_t;
using iocoro::unexpected;

// Import comparison operators so `rediscoro::operator==/!=` remains valid if referenced directly.
using iocoro::operator==;
using iocoro::operator!=;

}  // namespace rediscoro
