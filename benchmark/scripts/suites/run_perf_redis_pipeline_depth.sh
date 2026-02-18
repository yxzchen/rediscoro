#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/../run_perf_ratio_suite.sh" \
  --suite-name "redis_pipeline_depth benchmark suite" \
  --usage-name "benchmark/scripts/suites/run_perf_redis_pipeline_depth.sh" \
  --scenario-fields "sessions,total_cmds_per_session,pipeline_depth" \
  --scenario-format "sessions:total_cmds_per_session:pipeline_depth tuples" \
  --scenarios-default "8:80000:1,8:80000:8,8:80000:64,8:80000:256" \
  --rediscoro-target "rediscoro_redis_pipeline_depth" \
  --boostredis-target "boostredis_redis_pipeline_depth" \
  --metric-names "throughput_ops_s,p95_cmd_us" \
  --primary-metric "throughput_ops_s" \
  --ratio-mode "direct" \
  --ratio-field "ratio_vs_boostredis_throughput" \
  --run-timeout-default 120 \
  "$@"
