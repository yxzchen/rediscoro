#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "redis_latency benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_redis_latency.sh" \
  --scenario-fields "sessions,msgs,msg_bytes" \
  --scenario-format "sessions:msgs:msg_bytes tuples" \
  --scenarios-default "1:5000:16,8:1000:16,32:300:64" \
  --rediscoro-target "rediscoro_redis_latency" \
  --boostredis-target "boostredis_redis_latency" \
  --metric-names "p50_us,p95_us,p99_us" \
  --primary-metric "p95_us" \
  --ratio-mode "inverse" \
  --ratio-field "ratio_vs_boostredis_p95" \
  --ratio-label "boostredis_p95 / rediscoro_p95" \
  --run-timeout-default 90 \
  "$@"
