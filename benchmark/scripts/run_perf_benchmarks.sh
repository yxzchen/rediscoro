#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="$(cd -- "${BENCH_DIR}/.." && pwd)"

bench_require_non_negative_int() {
  local name="$1"
  local value="$2"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "$name must be a non-negative integer" >&2
    return 1
  fi
}

bench_require_positive_int() {
  local name="$1"
  local value="$2"
  bench_require_non_negative_int "$name" "$value" || return 1
  if [[ "$value" -le 0 ]]; then
    echo "$name must be > 0" >&2
    return 1
  fi
}

bench_to_abs_path() {
  local project_dir="$1"
  local path="$2"
  if [[ "$path" == /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s\n' "$project_dir/$path"
  fi
}

suite_fields_csv() {
  local suite_id="$1"
  case "$suite_id" in
    redis_latency)
      echo "sessions,msgs,msg_bytes"
      ;;
    redis_throughput)
      echo "sessions,total_ops_per_session,inflight"
      ;;
    redis_pipeline_depth)
      echo "sessions,total_cmds_per_session,pipeline_depth"
      ;;
    *)
      echo "Unknown suite id: $suite_id" >&2
      return 1
      ;;
  esac
}

field_in_list() {
  local field="$1"
  shift
  local item
  for item in "$@"; do
    if [[ "$item" == "$field" ]]; then
      return 0
    fi
  done
  return 1
}

trim_spaces() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s\n' "$s"
}

serialize_scenario_rows() {
  local suite_id="$1"
  local rows_name="$2"
  local out_name="$3"

  local -n rows="$rows_name"
  local -n out="$out_name"

  local fields_csv
  fields_csv="$(suite_fields_csv "$suite_id")" || return 1

  local -a fields=()
  IFS=',' read -r -a fields <<<"$fields_csv"

  if [[ ${#rows[@]} -eq 0 ]]; then
    echo "${suite_id}: SCENARIO_ROWS must not be empty" >&2
    return 1
  fi

  local serialized=""
  local idx=0
  local row_raw row token key value tuple
  for row_raw in "${rows[@]}"; do
    idx=$((idx + 1))
    row="$(trim_spaces "$row_raw")"

    if [[ -z "$row" ]]; then
      echo "${suite_id}: SCENARIO_ROWS[$idx] is empty" >&2
      return 1
    fi

    declare -A kv=()
    for token in $row; do
      if [[ "$token" != *=* ]]; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] invalid token: $token (expected key=value)" >&2
        return 1
      fi
      key="${token%%=*}"
      value="${token#*=}"
      if ! field_in_list "$key" "${fields[@]}"; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] unknown field: $key" >&2
        echo "Expected fields: $fields_csv" >&2
        return 1
      fi
      bench_require_positive_int "${suite_id}.SCENARIO_ROWS[$idx].$key" "$value"
      kv["$key"]="$value"
    done

    local -a ordered_values=()
    local f
    for f in "${fields[@]}"; do
      if [[ -z "${kv[$f]+x}" ]]; then
        echo "${suite_id}: SCENARIO_ROWS[$idx] missing field: $f" >&2
        echo "Expected fields: $fields_csv" >&2
        return 1
      fi
      ordered_values+=("${kv[$f]}")
    done

    tuple="$(IFS=':'; echo "${ordered_values[*]}")"
    serialized+="${serialized:+,}${tuple}"
  done

  out="$serialized"
}

load_suite_config() {
  local suite_id="$1"
  local conf_file="$2"
  local -n out_iterations="$3"
  local -n out_warmup="$4"
  local -n out_timeout_sec="$5"
  local -n out_scenarios="$6"

  if [[ ! -f "$conf_file" ]]; then
    echo "Missing config for $suite_id: $conf_file" >&2
    exit 1
  fi

  local ITERATIONS=""
  local WARMUP=""
  local TIMEOUT_SEC=""
  local SCENARIOS=""
  local -a SCENARIO_ROWS=()

  # shellcheck disable=SC1090
  source "$conf_file"

  if [[ -z "$ITERATIONS" || -z "$WARMUP" || -z "$TIMEOUT_SEC" ]]; then
    echo "Invalid config for $suite_id: $conf_file" >&2
    echo "Required keys: ITERATIONS, WARMUP, TIMEOUT_SEC, and SCENARIO_ROWS (or SCENARIOS for legacy format)" >&2
    exit 1
  fi

  bench_require_positive_int "${suite_id}.ITERATIONS" "$ITERATIONS"
  bench_require_non_negative_int "${suite_id}.WARMUP" "$WARMUP"
  bench_require_non_negative_int "${suite_id}.TIMEOUT_SEC" "$TIMEOUT_SEC"

  local scenarios_csv=""
  if [[ ${#SCENARIO_ROWS[@]} -gt 0 ]]; then
    serialize_scenario_rows "$suite_id" SCENARIO_ROWS scenarios_csv || exit 1
  elif [[ -n "$SCENARIOS" ]]; then
    scenarios_csv="$SCENARIOS"
  else
    echo "Invalid config for $suite_id: $conf_file" >&2
    echo "Missing scenario definition: set SCENARIO_ROWS (preferred) or SCENARIOS" >&2
    exit 1
  fi

  out_iterations="$ITERATIONS"
  out_warmup="$WARMUP"
  out_timeout_sec="$TIMEOUT_SEC"
  out_scenarios="$scenarios_csv"
}

run_step_with_summary() {
  local step_name="$1"
  local summary_file="$2"
  shift 2

  mkdir -p "$(dirname -- "$summary_file")"
  {
    echo "[$step_name] command: $*"
    "$@"
  } 2>&1 | tee "$summary_file"
}

BUILD_DIR="$PROJECT_DIR/build"
CONF_DIR="$PROJECT_DIR/benchmark/conf"
NO_SCHEMA_VALIDATE=false

ITERATIONS_OVERRIDE=""
WARMUP_OVERRIDE=""
TIMEOUT_SEC_OVERRIDE=""

REDIS_LATENCY_CONFIG=""
REDIS_THROUGHPUT_CONFIG=""
REDIS_PIPELINE_DEPTH_CONFIG=""

REDIS_LATENCY_SCENARIOS_OVERRIDE=""
REDIS_THROUGHPUT_SCENARIOS_OVERRIDE=""
REDIS_PIPELINE_DEPTH_SCENARIOS_OVERRIDE=""

REDIS_LATENCY_TIMEOUT_OVERRIDE=""
REDIS_THROUGHPUT_TIMEOUT_OVERRIDE=""
REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE=""

REDIS_LATENCY_REPORT="$PROJECT_DIR/benchmark/reports/redis_latency.report.json"
REDIS_THROUGHPUT_REPORT="$PROJECT_DIR/benchmark/reports/redis_throughput.report.json"
REDIS_PIPELINE_DEPTH_REPORT="$PROJECT_DIR/benchmark/reports/redis_pipeline_depth.report.json"

usage() {
  cat <<EOF2
Usage: benchmark/scripts/run_perf_benchmarks.sh [options]

Run all performance benchmark suites:
- redis_latency
- redis_throughput
- redis_pipeline_depth

Options:
  --build-dir DIR                               CMake build dir containing benchmark binaries (default: ./build)
  --conf-dir DIR                                Directory containing suite config files (default: benchmark/conf)
                                                 Config files use ITERATIONS/WARMUP/TIMEOUT_SEC/SCENARIO_ROWS.
  --iterations N                                Override ITERATIONS for all suites
  --warmup N                                    Override WARMUP for all suites
  --timeout-sec N                               Override TIMEOUT_SEC for all suites
  --no-schema-validate                          Skip JSON schema validation

  --redis-latency-config FILE                   Config file for redis_latency
  --redis-throughput-config FILE                Config file for redis_throughput
  --redis-pipeline-depth-config FILE            Config file for redis_pipeline_depth

  --redis-latency-scenarios LIST                Override scenarios CSV for redis_latency (e.g. 1:5000:16,8:1000:16)
  --redis-throughput-scenarios LIST             Override scenarios CSV for redis_throughput
  --redis-pipeline-depth-scenarios LIST         Override scenarios CSV for redis_pipeline_depth

  --redis-latency-timeout-sec N                 Override TIMEOUT_SEC for redis_latency
  --redis-throughput-timeout-sec N              Override TIMEOUT_SEC for redis_throughput
  --redis-pipeline-depth-timeout-sec N          Override TIMEOUT_SEC for redis_pipeline_depth

  --redis-latency-report FILE                   Report path (default: benchmark/reports/redis_latency.report.json)
  --redis-throughput-report FILE                Report path (default: benchmark/reports/redis_throughput.report.json)
  --redis-pipeline-depth-report FILE            Report path (default: benchmark/reports/redis_pipeline_depth.report.json)

  -h, --help                                    Show this help
EOF2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --conf-dir)
      CONF_DIR="$2"
      shift 2
      ;;
    --iterations)
      ITERATIONS_OVERRIDE="$2"
      shift 2
      ;;
    --warmup)
      WARMUP_OVERRIDE="$2"
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC_OVERRIDE="$2"
      shift 2
      ;;
    --no-schema-validate)
      NO_SCHEMA_VALIDATE=true
      shift
      ;;

    --redis-latency-config)
      REDIS_LATENCY_CONFIG="$2"
      shift 2
      ;;
    --redis-throughput-config)
      REDIS_THROUGHPUT_CONFIG="$2"
      shift 2
      ;;
    --redis-pipeline-depth-config)
      REDIS_PIPELINE_DEPTH_CONFIG="$2"
      shift 2
      ;;

    --redis-latency-scenarios)
      REDIS_LATENCY_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --redis-throughput-scenarios)
      REDIS_THROUGHPUT_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;
    --redis-pipeline-depth-scenarios)
      REDIS_PIPELINE_DEPTH_SCENARIOS_OVERRIDE="$2"
      shift 2
      ;;

    --redis-latency-timeout-sec)
      REDIS_LATENCY_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --redis-throughput-timeout-sec)
      REDIS_THROUGHPUT_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --redis-pipeline-depth-timeout-sec)
      REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;

    --redis-latency-report)
      REDIS_LATENCY_REPORT="$2"
      shift 2
      ;;
    --redis-throughput-report)
      REDIS_THROUGHPUT_REPORT="$2"
      shift 2
      ;;
    --redis-pipeline-depth-report)
      REDIS_PIPELINE_DEPTH_REPORT="$2"
      shift 2
      ;;

    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "$ITERATIONS_OVERRIDE" ]]; then
  bench_require_positive_int "--iterations" "$ITERATIONS_OVERRIDE"
fi
if [[ -n "$WARMUP_OVERRIDE" ]]; then
  bench_require_non_negative_int "--warmup" "$WARMUP_OVERRIDE"
fi
if [[ -n "$TIMEOUT_SEC_OVERRIDE" ]]; then
  bench_require_non_negative_int "--timeout-sec" "$TIMEOUT_SEC_OVERRIDE"
fi
if [[ -n "$REDIS_LATENCY_TIMEOUT_OVERRIDE" ]]; then
  bench_require_non_negative_int "--redis-latency-timeout-sec" "$REDIS_LATENCY_TIMEOUT_OVERRIDE"
fi
if [[ -n "$REDIS_THROUGHPUT_TIMEOUT_OVERRIDE" ]]; then
  bench_require_non_negative_int "--redis-throughput-timeout-sec" "$REDIS_THROUGHPUT_TIMEOUT_OVERRIDE"
fi
if [[ -n "$REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE" ]]; then
  bench_require_non_negative_int "--redis-pipeline-depth-timeout-sec" "$REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE"
fi

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"
CONF_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$CONF_DIR")"

: "${REDIS_LATENCY_CONFIG:=$CONF_DIR/redis_latency.conf}"
: "${REDIS_THROUGHPUT_CONFIG:=$CONF_DIR/redis_throughput.conf}"
: "${REDIS_PIPELINE_DEPTH_CONFIG:=$CONF_DIR/redis_pipeline_depth.conf}"

REDIS_LATENCY_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_LATENCY_CONFIG")"
REDIS_THROUGHPUT_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_THROUGHPUT_CONFIG")"
REDIS_PIPELINE_DEPTH_CONFIG="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_PIPELINE_DEPTH_CONFIG")"

REDIS_LATENCY_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_LATENCY_REPORT")"
REDIS_THROUGHPUT_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_THROUGHPUT_REPORT")"
REDIS_PIPELINE_DEPTH_REPORT="$(bench_to_abs_path "$PROJECT_DIR" "$REDIS_PIPELINE_DEPTH_REPORT")"

load_suite_config "redis_latency" "$REDIS_LATENCY_CONFIG" \
  cfg_latency_iterations cfg_latency_warmup cfg_latency_timeout cfg_latency_scenarios
load_suite_config "redis_throughput" "$REDIS_THROUGHPUT_CONFIG" \
  cfg_throughput_iterations cfg_throughput_warmup cfg_throughput_timeout cfg_throughput_scenarios
load_suite_config "redis_pipeline_depth" "$REDIS_PIPELINE_DEPTH_CONFIG" \
  cfg_pipeline_iterations cfg_pipeline_warmup cfg_pipeline_timeout cfg_pipeline_scenarios

latency_iterations="$cfg_latency_iterations"
throughput_iterations="$cfg_throughput_iterations"
pipeline_iterations="$cfg_pipeline_iterations"

latency_warmup="$cfg_latency_warmup"
throughput_warmup="$cfg_throughput_warmup"
pipeline_warmup="$cfg_pipeline_warmup"

latency_timeout="$cfg_latency_timeout"
throughput_timeout="$cfg_throughput_timeout"
pipeline_timeout="$cfg_pipeline_timeout"

latency_scenarios="$cfg_latency_scenarios"
throughput_scenarios="$cfg_throughput_scenarios"
pipeline_scenarios="$cfg_pipeline_scenarios"

if [[ -n "$ITERATIONS_OVERRIDE" ]]; then
  latency_iterations="$ITERATIONS_OVERRIDE"
  throughput_iterations="$ITERATIONS_OVERRIDE"
  pipeline_iterations="$ITERATIONS_OVERRIDE"
fi
if [[ -n "$WARMUP_OVERRIDE" ]]; then
  latency_warmup="$WARMUP_OVERRIDE"
  throughput_warmup="$WARMUP_OVERRIDE"
  pipeline_warmup="$WARMUP_OVERRIDE"
fi
if [[ -n "$TIMEOUT_SEC_OVERRIDE" ]]; then
  latency_timeout="$TIMEOUT_SEC_OVERRIDE"
  throughput_timeout="$TIMEOUT_SEC_OVERRIDE"
  pipeline_timeout="$TIMEOUT_SEC_OVERRIDE"
fi

if [[ -n "$REDIS_LATENCY_TIMEOUT_OVERRIDE" ]]; then latency_timeout="$REDIS_LATENCY_TIMEOUT_OVERRIDE"; fi
if [[ -n "$REDIS_THROUGHPUT_TIMEOUT_OVERRIDE" ]]; then throughput_timeout="$REDIS_THROUGHPUT_TIMEOUT_OVERRIDE"; fi
if [[ -n "$REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE" ]]; then pipeline_timeout="$REDIS_PIPELINE_DEPTH_TIMEOUT_OVERRIDE"; fi

if [[ -n "$REDIS_LATENCY_SCENARIOS_OVERRIDE" ]]; then latency_scenarios="$REDIS_LATENCY_SCENARIOS_OVERRIDE"; fi
if [[ -n "$REDIS_THROUGHPUT_SCENARIOS_OVERRIDE" ]]; then throughput_scenarios="$REDIS_THROUGHPUT_SCENARIOS_OVERRIDE"; fi
if [[ -n "$REDIS_PIPELINE_DEPTH_SCENARIOS_OVERRIDE" ]]; then pipeline_scenarios="$REDIS_PIPELINE_DEPTH_SCENARIOS_OVERRIDE"; fi

REDIS_LATENCY_SUMMARY="$(dirname -- "$REDIS_LATENCY_REPORT")/redis_latency.summary.txt"
REDIS_THROUGHPUT_SUMMARY="$(dirname -- "$REDIS_THROUGHPUT_REPORT")/redis_throughput.summary.txt"
REDIS_PIPELINE_DEPTH_SUMMARY="$(dirname -- "$REDIS_PIPELINE_DEPTH_REPORT")/redis_pipeline_depth.summary.txt"

suite_redis_latency_cmd=(
  "$SCRIPT_DIR/suites/run_perf_redis_latency.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$latency_iterations"
  --warmup "$latency_warmup"
  --run-timeout-sec "$latency_timeout"
  --scenarios "$latency_scenarios"
  --report "$REDIS_LATENCY_REPORT"
)

suite_redis_throughput_cmd=(
  "$SCRIPT_DIR/suites/run_perf_redis_throughput.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$throughput_iterations"
  --warmup "$throughput_warmup"
  --run-timeout-sec "$throughput_timeout"
  --scenarios "$throughput_scenarios"
  --report "$REDIS_THROUGHPUT_REPORT"
)

suite_redis_pipeline_depth_cmd=(
  "$SCRIPT_DIR/suites/run_perf_redis_pipeline_depth.sh"
  --build-dir "$BUILD_DIR"
  --iterations "$pipeline_iterations"
  --warmup "$pipeline_warmup"
  --run-timeout-sec "$pipeline_timeout"
  --scenarios "$pipeline_scenarios"
  --report "$REDIS_PIPELINE_DEPTH_REPORT"
)

echo "Running performance benchmark suites"
echo "  build-dir: $BUILD_DIR"
echo "  conf-dir:  $CONF_DIR"
echo

run_step_with_summary "suite_redis_latency" "$REDIS_LATENCY_SUMMARY" "${suite_redis_latency_cmd[@]}"
run_step_with_summary "suite_redis_throughput" "$REDIS_THROUGHPUT_SUMMARY" "${suite_redis_throughput_cmd[@]}"
run_step_with_summary "suite_redis_pipeline_depth" "$REDIS_PIPELINE_DEPTH_SUMMARY" "${suite_redis_pipeline_depth_cmd[@]}"

if [[ "$NO_SCHEMA_VALIDATE" != true ]]; then
  python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/redis_latency.schema.json" \
    --report "$REDIS_LATENCY_REPORT"

  python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/redis_throughput.schema.json" \
    --report "$REDIS_THROUGHPUT_REPORT"

  python3 "$SCRIPT_DIR/validate_benchmark_report.py" \
    --schema "$PROJECT_DIR/benchmark/schemas/redis_pipeline_depth.schema.json" \
    --report "$REDIS_PIPELINE_DEPTH_REPORT"
fi

echo
echo "Benchmark run completed."
echo "  report:  $REDIS_LATENCY_REPORT"
echo "  report:  $REDIS_THROUGHPUT_REPORT"
echo "  report:  $REDIS_PIPELINE_DEPTH_REPORT"
echo "  summary: $REDIS_LATENCY_SUMMARY"
echo "  summary: $REDIS_THROUGHPUT_SUMMARY"
echo "  summary: $REDIS_PIPELINE_DEPTH_SUMMARY"
