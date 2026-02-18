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

bench_run_cmd_with_timeout() {
  local timeout_sec="$1"
  shift

  local out
  local code=0
  if [[ "$timeout_sec" -gt 0 ]] && command -v timeout >/dev/null 2>&1; then
    set +e
    out="$(timeout --foreground "${timeout_sec}s" "$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      if [[ $code -eq 124 ]]; then
        echo "Benchmark timed out after ${timeout_sec}s: $*" >&2
      else
        echo "Benchmark command failed ($code): $*" >&2
        echo "$out" >&2
      fi
      return 1
    fi
  else
    set +e
    out="$("$@" 2>&1)"
    code=$?
    set -e
    if [[ $code -ne 0 ]]; then
      echo "Benchmark command failed ($code): $*" >&2
      echo "$out" >&2
      return 1
    fi
  fi
  printf '%s\n' "$out"
}

bench_extract_metric() {
  local metric="$1"
  local line="$2"
  sed -nE "s/.* ${metric}=([0-9]+([.][0-9]+)?).*/\\1/p" <<<"$line"
}

bench_median_from_stdin() {
  awk '
    { vals[++n] = $1; }
    END {
      if (n == 0) { print "0"; exit; }
      mid = int((n + 1) / 2);
      if (n % 2 == 1) {
        printf "%.2f\n", vals[mid];
      } else {
        printf "%.2f\n", (vals[mid] + vals[mid + 1]) / 2.0;
      }
    }
  '
}

bench_compute_ratio() {
  local lhs="$1"
  local rhs="$2"
  awk -v l="$lhs" -v r="$rhs" 'BEGIN { if (r <= 0) { print "0.00"; } else { printf "%.4f\n", l / r; } }'
}

SUITE_NAME=""
USAGE_NAME="benchmark/scripts/run_perf_ratio_suite.sh"
SCENARIO_FIELDS_CSV=""
SCENARIO_FORMAT=""
SCENARIOS_DEFAULT=""
REDISCORO_TARGET=""
BOOSTREDIS_TARGET=""
METRIC_NAME=""
METRIC_NAMES_CSV=""
PRIMARY_METRIC=""
RATIO_MODE="direct"
RATIO_FIELD="ratio_vs_boostredis"
RATIO_LABEL=""
RUN_TIMEOUT_DEFAULT=120

BUILD_DIR="$PROJECT_DIR/build"
ITERATIONS=5
WARMUP=1
SCENARIOS=""
REPORT_FILE=""
RUN_TIMEOUT_SEC=""

usage() {
  cat <<EOF2
Usage: ${USAGE_NAME} [options]

Options:
  --build-dir DIR      CMake build dir containing benchmark binaries (default: ./build)
  --iterations N       Measured runs per scenario (default: 5)
  --warmup N           Warmup runs per scenario/framework (default: 1)
  --scenarios LIST     Comma-separated ${SCENARIO_FORMAT:-scenario tuples}
                       (default: ${SCENARIOS_DEFAULT:-none})
  --report FILE        Write JSON summary to FILE
  --run-timeout-sec N  Timeout for each benchmark process in seconds (default: ${RUN_TIMEOUT_DEFAULT}, 0=disable)
  -h, --help           Show this help
EOF2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --suite-name)
      SUITE_NAME="$2"
      shift 2
      ;;
    --usage-name)
      USAGE_NAME="$2"
      shift 2
      ;;
    --scenario-fields)
      SCENARIO_FIELDS_CSV="$2"
      shift 2
      ;;
    --scenario-format)
      SCENARIO_FORMAT="$2"
      shift 2
      ;;
    --scenarios-default)
      SCENARIOS_DEFAULT="$2"
      shift 2
      ;;
    --rediscoro-target)
      REDISCORO_TARGET="$2"
      shift 2
      ;;
    --boostredis-target)
      BOOSTREDIS_TARGET="$2"
      shift 2
      ;;
    --metric-name)
      METRIC_NAME="$2"
      shift 2
      ;;
    --metric-names)
      METRIC_NAMES_CSV="$2"
      shift 2
      ;;
    --primary-metric)
      PRIMARY_METRIC="$2"
      shift 2
      ;;
    --ratio-mode)
      RATIO_MODE="$2"
      shift 2
      ;;
    --ratio-field)
      RATIO_FIELD="$2"
      shift 2
      ;;
    --ratio-label)
      RATIO_LABEL="$2"
      shift 2
      ;;
    --run-timeout-default)
      RUN_TIMEOUT_DEFAULT="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --iterations)
      ITERATIONS="$2"
      shift 2
      ;;
    --warmup)
      WARMUP="$2"
      shift 2
      ;;
    --scenarios)
      SCENARIOS="$2"
      shift 2
      ;;
    --report)
      REPORT_FILE="$2"
      shift 2
      ;;
    --run-timeout-sec)
      RUN_TIMEOUT_SEC="$2"
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

if [[ -z "$SUITE_NAME" || -z "$SCENARIO_FIELDS_CSV" || -z "$REDISCORO_TARGET" || -z "$BOOSTREDIS_TARGET" ]]; then
  echo "Missing required suite configuration in wrapper script" >&2
  exit 1
fi

if [[ -z "$METRIC_NAMES_CSV" ]]; then
  METRIC_NAMES_CSV="$METRIC_NAME"
fi
if [[ -z "$METRIC_NAMES_CSV" ]]; then
  echo "Missing metric configuration: use --metric-name or --metric-names" >&2
  exit 1
fi

IFS=',' read -r -a METRIC_NAMES <<<"$METRIC_NAMES_CSV"
if [[ ${#METRIC_NAMES[@]} -eq 0 ]]; then
  echo "--metric-names must not be empty" >&2
  exit 1
fi
for metric in "${METRIC_NAMES[@]}"; do
  if [[ -z "$metric" ]]; then
    echo "--metric-names contains empty entry" >&2
    exit 1
  fi
done

if [[ -z "$PRIMARY_METRIC" ]]; then
  PRIMARY_METRIC="${METRIC_NAMES[0]}"
fi
primary_ok=false
for metric in "${METRIC_NAMES[@]}"; do
  if [[ "$metric" == "$PRIMARY_METRIC" ]]; then
    primary_ok=true
    break
  fi
done
if [[ "$primary_ok" != true ]]; then
  echo "--primary-metric must be one of: $METRIC_NAMES_CSV" >&2
  exit 1
fi

if [[ -z "$SCENARIOS" ]]; then
  SCENARIOS="$SCENARIOS_DEFAULT"
fi
if [[ -z "$SCENARIOS" ]]; then
  echo "--scenarios is required" >&2
  exit 1
fi

if [[ -z "$RUN_TIMEOUT_SEC" ]]; then
  RUN_TIMEOUT_SEC="$RUN_TIMEOUT_DEFAULT"
fi

if [[ "$RATIO_MODE" != "direct" && "$RATIO_MODE" != "inverse" ]]; then
  echo "--ratio-mode must be one of: direct, inverse" >&2
  exit 1
fi

bench_require_positive_int "--iterations" "$ITERATIONS"
bench_require_non_negative_int "--warmup" "$WARMUP"
bench_require_non_negative_int "--run-timeout-sec" "$RUN_TIMEOUT_SEC"

BUILD_DIR="$(bench_to_abs_path "$PROJECT_DIR" "$BUILD_DIR")"
REDISCORO_BIN="$BUILD_DIR/benchmark/$REDISCORO_TARGET"
BOOSTREDIS_BIN="$BUILD_DIR/benchmark/$BOOSTREDIS_TARGET"

if [[ ! -x "$REDISCORO_BIN" ]]; then
  echo "Missing benchmark binary: $REDISCORO_BIN" >&2
  exit 1
fi
if [[ ! -x "$BOOSTREDIS_BIN" ]]; then
  echo "Missing benchmark binary: $BOOSTREDIS_BIN" >&2
  exit 1
fi

if [[ -n "$REPORT_FILE" ]]; then
  REPORT_FILE="$(bench_to_abs_path "$PROJECT_DIR" "$REPORT_FILE")"
fi

IFS=',' read -r -a SCENARIO_FIELDS <<<"$SCENARIO_FIELDS_CSV"
SCENARIO_ARITY=${#SCENARIO_FIELDS[@]}
if [[ "$SCENARIO_ARITY" -le 0 ]]; then
  echo "--scenario-fields must not be empty" >&2
  exit 1
fi

table_rows=()
scenario_json=()

ratio_expr="rediscoro/boostredis"
if [[ "$RATIO_MODE" == "inverse" ]]; then
  ratio_expr="boostredis/rediscoro"
fi
if [[ -n "$RATIO_LABEL" ]]; then
  ratio_expr="$RATIO_LABEL"
fi

echo "Running $SUITE_NAME"
echo "  rediscoro : $REDISCORO_BIN"
echo "  boostredis: $BOOSTREDIS_BIN"
echo "  scenarios: $SCENARIOS"
echo "  warmup: $WARMUP, measured iterations: $ITERATIONS"
echo "  per-run timeout: ${RUN_TIMEOUT_SEC}s"
echo

IFS=',' read -r -a SCENARIO_ITEMS <<<"$SCENARIOS"
for item in "${SCENARIO_ITEMS[@]}"; do
  IFS=':' read -r -a RAW_VALUES <<<"$item"
  if [[ ${#RAW_VALUES[@]} -ne "$SCENARIO_ARITY" ]]; then
    echo "Invalid scenario: $item (expected $SCENARIO_FORMAT)" >&2
    exit 1
  fi

  VALUES=("${RAW_VALUES[@]}")
  for value in "${VALUES[@]}"; do
    if [[ -z "$value" ]]; then
      echo "Invalid scenario: $item (empty field)" >&2
      exit 1
    fi
  done

  scenario_desc=()
  for ((idx = 0; idx < SCENARIO_ARITY; ++idx)); do
    scenario_desc+=("${SCENARIO_FIELDS[$idx]}=${VALUES[$idx]}")
  done
  echo "Scenario ${scenario_desc[*]}"

  for ((i = 0; i < WARMUP; ++i)); do
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$REDISCORO_BIN" "${VALUES[@]}" >/dev/null
    bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$BOOSTREDIS_BIN" "${VALUES[@]}" >/dev/null
  done

  for metric in "${METRIC_NAMES[@]}"; do
    eval "rediscoro_runs_${metric}=()"
    eval "boostredis_runs_${metric}=()"
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$REDISCORO_BIN" "${VALUES[@]}")"
    for metric in "${METRIC_NAMES[@]}"; do
      metric_value="$(bench_extract_metric "$metric" "$line")"
      if [[ -z "$metric_value" ]]; then
        echo "Failed to parse rediscoro $metric: $line" >&2
        exit 1
      fi
      eval "rediscoro_runs_${metric}+=(\"$metric_value\")"
    done
  done

  for ((i = 0; i < ITERATIONS; ++i)); do
    line="$(bench_run_cmd_with_timeout "$RUN_TIMEOUT_SEC" "$BOOSTREDIS_BIN" "${VALUES[@]}")"
    for metric in "${METRIC_NAMES[@]}"; do
      metric_value="$(bench_extract_metric "$metric" "$line")"
      if [[ -z "$metric_value" ]]; then
        echo "Failed to parse boostredis $metric: $line" >&2
        exit 1
      fi
      eval "boostredis_runs_${metric}+=(\"$metric_value\")"
    done
  done

  declare -A rediscoro_medians=()
  declare -A boostredis_medians=()
  for metric in "${METRIC_NAMES[@]}"; do
    eval "rediscoro_values=(\"\${rediscoro_runs_${metric}[@]}\")"
    rediscoro_medians["$metric"]="$(printf '%s\n' "${rediscoro_values[@]}" | sort -n | bench_median_from_stdin)"

    eval "boostredis_values=(\"\${boostredis_runs_${metric}[@]}\")"
    boostredis_medians["$metric"]="$(printf '%s\n' "${boostredis_values[@]}" | sort -n | bench_median_from_stdin)"
  done

  primary_rediscoro="${rediscoro_medians[$PRIMARY_METRIC]}"
  primary_boostredis="${boostredis_medians[$PRIMARY_METRIC]}"

  if [[ "$RATIO_MODE" == "direct" ]]; then
    ratio="$(bench_compute_ratio "$primary_rediscoro" "$primary_boostredis")"
  else
    ratio="$(bench_compute_ratio "$primary_boostredis" "$primary_rediscoro")"
  fi

  row_prefix="$(IFS='|'; echo "${VALUES[*]}")"
  table_rows+=("${row_prefix}|${primary_rediscoro}|${primary_boostredis}|${ratio}")

  scenario_json_entry="{"
  for ((idx = 0; idx < SCENARIO_ARITY; ++idx)); do
    scenario_json_entry+="\"${SCENARIO_FIELDS[$idx]}\":${VALUES[$idx]},"
  done

  for metric in "${METRIC_NAMES[@]}"; do
    eval "rediscoro_values=(\"\${rediscoro_runs_${metric}[@]}\")"
    eval "boostredis_values=(\"\${boostredis_runs_${metric}[@]}\")"
    rediscoro_runs_json="$(printf '%s\n' "${rediscoro_values[@]}" | paste -sd, -)"
    boostredis_runs_json="$(printf '%s\n' "${boostredis_values[@]}" | paste -sd, -)"
    scenario_json_entry+="\"rediscoro_${metric}_runs\":[${rediscoro_runs_json}],"
    scenario_json_entry+="\"boostredis_${metric}_runs\":[${boostredis_runs_json}],"
    scenario_json_entry+="\"rediscoro_${metric}_median\":${rediscoro_medians[$metric]},"
    scenario_json_entry+="\"boostredis_${metric}_median\":${boostredis_medians[$metric]},"
  done

  scenario_json_entry+="\"${RATIO_FIELD}\":${ratio}}"
  scenario_json+=("$scenario_json_entry")

  if [[ ${#METRIC_NAMES[@]} -eq 1 ]]; then
    metric="${METRIC_NAMES[0]}"
    echo "  rediscoro median $metric: ${rediscoro_medians[$metric]}"
    echo "  boostredis median $metric: ${boostredis_medians[$metric]}"
  else
    rediscoro_line=""
    boostredis_line=""
    for metric in "${METRIC_NAMES[@]}"; do
      rediscoro_line+="$metric=${rediscoro_medians[$metric]} "
      boostredis_line+="$metric=${boostredis_medians[$metric]} "
    done
    echo "  rediscoro medians: ${rediscoro_line% }"
    echo "  boostredis medians: ${boostredis_line% }"
  fi
  echo "  ratio ($ratio_expr): $ratio"
  echo
done

echo "Summary"
header=("${SCENARIO_FIELDS[@]}" "rediscoro_${PRIMARY_METRIC}_median" "boostredis_${PRIMARY_METRIC}_median" "$RATIO_FIELD")
printf '|'
for col in "${header[@]}"; do
  printf ' %s |' "$col"
done
printf '\n|'
for _ in "${header[@]}"; do
  printf ' --- |'
done
printf '\n'
for row in "${table_rows[@]}"; do
  IFS='|' read -r -a cols <<<"$row"
  printf '|'
  for col in "${cols[@]}"; do
    printf ' %s |' "$col"
  done
  printf '\n'
done
echo

if [[ -n "$REPORT_FILE" ]]; then
  mkdir -p "$(dirname -- "$REPORT_FILE")"
  timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  {
    printf '{\n'
    printf '  "schema_version": 1,\n'
    printf '  "timestamp_utc": "%s",\n' "$timestamp"
    printf '  "build_dir": "%s",\n' "$BUILD_DIR"
    printf '  "iterations": %s,\n' "$ITERATIONS"
    printf '  "warmup": %s,\n' "$WARMUP"
    printf '  "scenarios": [\n'
    for ((i = 0; i < ${#scenario_json[@]}; ++i)); do
      suffix=","
      if [[ $i -eq $(( ${#scenario_json[@]} - 1 )) ]]; then
        suffix=""
      fi
      printf '    %s%s\n' "${scenario_json[$i]}" "$suffix"
    done
    printf '  ]\n'
    printf '}\n'
  } >"$REPORT_FILE"
  echo "Wrote report: $REPORT_FILE"
fi
