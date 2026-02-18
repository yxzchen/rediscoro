#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "redis_throughput benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_redis_throughput.sh" \
  --scenario-fields "sessions,total_ops_per_session,inflight" \
  --scenario-format "sessions:total_ops_per_session:inflight tuples" \
  --scenarios-default "1:200000:1,8:50000:8,32:20000:32" \
  --rediscoro-target "rediscoro_redis_throughput" \
  --boostredis-target "boostredis_redis_throughput" \
  --metric-name "throughput_ops_s" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_boostredis" \
  --run-timeout-default 120 \
  "$@"
