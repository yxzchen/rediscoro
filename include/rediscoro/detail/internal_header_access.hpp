#pragma once

// Access gate for rediscoro internal headers (<rediscoro/detail/*.hpp>).
//
// Allowed include paths:
// - Through public rediscoro headers (they define REDISCORO_DETAIL_INTERNAL_INCLUDE)
// - Explicit opt-in by advanced users/tests (define REDISCORO_ALLOW_INTERNAL_HEADERS)
#if !defined(REDISCORO_DETAIL_INTERNAL_INCLUDE) && !defined(REDISCORO_ALLOW_INTERNAL_HEADERS)
#error \
  "rediscoro internal header is not part of the public API. Include <rediscoro/...> public headers, or define REDISCORO_ALLOW_INTERNAL_HEADERS to opt in."
#endif
