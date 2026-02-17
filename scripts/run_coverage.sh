#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="build-coverage"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=false
MIN_LINE_COVERAGE=70.0
MIN_BRANCH_COVERAGE=""
MIN_FUNCTION_COVERAGE=""

print_help() {
  cat << 'EOF'
Usage: ./scripts/run_coverage.sh [OPTIONS]

Options:
  --build-dir DIR    Build directory (default: build-coverage)
  --jobs N           Parallel build/test jobs (default: auto-detected)
  --min-line PCT     Minimum line coverage percent, must be exceeded (default: 70.0)
  --min-branch PCT   Minimum branch coverage percent, must be exceeded (default: disabled)
  --min-function PCT Minimum function coverage percent, must be exceeded (default: disabled)
  --clean            Remove build directory before configuring
  -h, --help         Show this help

Artifacts:
  <build-dir>/coverage/summary.txt
  <build-dir>/coverage/summary.json
  <build-dir>/coverage/coverage.xml
  <build-dir>/coverage/index.html
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    --min-line)
      MIN_LINE_COVERAGE="$2"
      shift 2
      ;;
    --min-branch)
      MIN_BRANCH_COVERAGE="$2"
      shift 2
      ;;
    --min-function)
      MIN_FUNCTION_COVERAGE="$2"
      shift 2
      ;;
    --clean)
      CLEAN=true
      shift
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    *)
      echo "[ERROR] Unknown option: $1" >&2
      print_help
      exit 1
      ;;
  esac
done

if ! command -v cmake >/dev/null 2>&1; then
  echo "[ERROR] cmake is required" >&2
  exit 1
fi

if ! command -v gcovr >/dev/null 2>&1; then
  echo "[ERROR] gcovr is required (apt install gcovr)" >&2
  exit 1
fi

if [[ "$CLEAN" == true ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DREDISCORO_BUILD_TESTS=ON \
  -DREDISCORO_BUILD_EXAMPLES=OFF \
  -DREDISCORO_ENABLE_COVERAGE=ON

cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS"

OUT_DIR="$BUILD_DIR/coverage"
mkdir -p "$OUT_DIR"

COMMON_ARGS=(
  --root .
  --object-directory "$BUILD_DIR"
  --filter 'include/rediscoro/.*'
  --exclude '.*/test/.*'
  --exclude '.*/examples/.*'
  --exclude '.*/build.*/.*'
  --gcov-ignore-parse-errors negative_hits.warn_once_per_file
)

gcovr "${COMMON_ARGS[@]}" --txt "$OUT_DIR/summary.txt" --print-summary
gcovr "${COMMON_ARGS[@]}" --json-summary-pretty --json-summary "$OUT_DIR/summary.json"
gcovr "${COMMON_ARGS[@]}" --xml-pretty --xml "$OUT_DIR/coverage.xml"
gcovr "${COMMON_ARGS[@]}" --html-details "$OUT_DIR/index.html"

python3 - "$OUT_DIR/summary.json" <<'PY'
import json
import pathlib
import sys

summary_path = pathlib.Path(sys.argv[1])
with summary_path.open("r", encoding="utf-8") as f:
    data = json.load(f)

line = data.get("line_percent", 0.0)
branch = data.get("branch_percent", 0.0)
func = data.get("function_percent", 0.0)
print(f"[INFO] Coverage summary: line={line:.2f}% branch={branch:.2f}% function={func:.2f}%")
PY

run_gate_check() {
  local metric_key="$1"
  local metric_label="$2"
  local threshold="$3"
  python3 - "$OUT_DIR/summary.json" "$metric_key" "$metric_label" "$threshold" <<'PY'
import json
import pathlib
import sys

summary_path = pathlib.Path(sys.argv[1])
metric_key = sys.argv[2]
metric_label = sys.argv[3]
threshold = float(sys.argv[4])
with summary_path.open("r", encoding="utf-8") as f:
    data = json.load(f)

value = float(data.get(metric_key, 0.0))
if value <= threshold:
    print(
        f"[ERROR] {metric_label} coverage gate failed: {value:.2f}% is not greater than {threshold:.2f}%",
        file=sys.stderr,
    )
    sys.exit(1)
print(f"[INFO] {metric_label} coverage gate passed: {value:.2f}% > {threshold:.2f}%")
PY
}

run_gate_check "line_percent" "Line" "$MIN_LINE_COVERAGE"

if [[ -n "$MIN_BRANCH_COVERAGE" ]]; then
  run_gate_check "branch_percent" "Branch" "$MIN_BRANCH_COVERAGE"
fi

if [[ -n "$MIN_FUNCTION_COVERAGE" ]]; then
  run_gate_check "function_percent" "Function" "$MIN_FUNCTION_COVERAGE"
fi
echo "[INFO] Coverage reports written to: $OUT_DIR"
