# Benchmarks

This directory provides Redis performance benchmarks comparing:

- `rediscoro`
- `boost::redis` (Boost 1.89 via CMake FetchContent)

## Suites

- `redis_latency`: round-trip latency (`ECHO`) with p50/p95/p99.
- `redis_throughput`: command throughput (`PING`) under configurable concurrency and in-flight batch.
- `redis_pipeline_depth`: impact of pipeline batch size on throughput and per-command p95 latency.

## Build

```bash
cmake -S . -B build-bench \
  -DREDISCORO_BUILD_BENCHMARKS=ON \
  -DREDISCORO_BUILD_TESTS=OFF \
  -DREDISCORO_BUILD_EXAMPLES=OFF
cmake --build build-bench -j
```

## Run All

```bash
./benchmark/scripts/run_perf_benchmarks.sh --build-dir build-bench
```

## Common Overrides

```bash
./benchmark/scripts/run_perf_benchmarks.sh \
  --build-dir build-bench \
  --iterations 5 \
  --warmup 1 \
  --timeout-sec 180
```

## Config Format

Each suite config in `benchmark/conf/*.conf` uses:

- `ITERATIONS`
- `WARMUP`
- `TIMEOUT_SEC`
- `SCENARIO_ROWS` (preferred, iocoro-style), for example:

```bash
SCENARIO_ROWS=(
  "sessions=1 msgs=5000 msg_bytes=16"
  "sessions=8 msgs=1000 msg_bytes=16"
)
```

## Outputs

Generated files are written under `benchmark/reports/`:

- `redis_latency.report.json`
- `redis_throughput.report.json`
- `redis_pipeline_depth.report.json`
- matching `*.summary.txt`
