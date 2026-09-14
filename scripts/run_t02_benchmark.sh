#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$PROJECT_DIR/build/camera_detection"
INPUT="$PROJECT_DIR/test_data/blue_ball_basic.mp4"
RESULTS_ROOT="$PROJECT_DIR/results/t02-$(date -u +%Y%m%dT%H%M%SZ)"
RUNS=3
WARMUP_FRAMES=30
MODES=(unpaced realtime)
EXTRA_ARGS=()

usage() {
	cat <<'EOF'
Usage: scripts/run_t02_benchmark.sh [options] [-- camera_detection args...]

Options:
  --bin=PATH             camera_detection binary (default: ./build/camera_detection)
  --input=PATH           fixed input video
  --results-root=PATH    output parent directory
  --runs=N               repetitions per replay mode (default: 3)
  --warmup-frames=N      frames excluded by analysis (default: 30)
  --modes=LIST           comma-separated unpaced,realtime (default: both)
  -h, --help             show this help

The benchmark always disables display and streaming. WebRTC needs a separate
realtime run with an active browser session.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--bin=*) BIN="${1#*=}" ;;
		--input=*) INPUT="${1#*=}" ;;
		--results-root=*) RESULTS_ROOT="${1#*=}" ;;
		--runs=*) RUNS="${1#*=}" ;;
		--warmup-frames=*) WARMUP_FRAMES="${1#*=}" ;;
		--modes=*)
			IFS=',' read -r -a MODES <<< "${1#*=}"
			;;
		-h|--help)
			usage
			exit 0
			;;
		--)
			shift
			EXTRA_ARGS=("$@")
			break
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage
			exit 1
			;;
	esac
	shift
done

if ! [[ "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
	echo "Invalid --runs value: $RUNS" >&2
	exit 1
fi
if ! [[ "$WARMUP_FRAMES" =~ ^[0-9]+$ ]]; then
	echo "Invalid --warmup-frames value: $WARMUP_FRAMES" >&2
	exit 1
fi
if [[ ! -x "$BIN" ]]; then
	echo "Binary not found or not executable: $BIN" >&2
	exit 1
fi
if [[ ! -f "$INPUT" ]]; then
	echo "Input file not found: $INPUT" >&2
	exit 1
fi

for mode in "${MODES[@]}"; do
	if [[ "$mode" != "unpaced" && "$mode" != "realtime" ]]; then
		echo "Unsupported replay mode: $mode" >&2
		exit 1
	fi
done

mkdir -p "$RESULTS_ROOT"
RUN_DIRS=()
for mode in "${MODES[@]}"; do
	for ((run=1; run<=RUNS; run++)); do
		run_dir="$RESULTS_ROOT/${mode}-run${run}"
		echo "T02 run: mode=$mode run=$run/$RUNS output=$run_dir"
		"$BIN" \
			--input="$INPUT" \
			--replay="$mode" \
			--no-display \
			--no-stream \
			--metrics-dir="$run_dir" \
			"${EXTRA_ARGS[@]}"
		RUN_DIRS+=("$run_dir")
	done
done

python3 "$PROJECT_DIR/scripts/analyze_metrics.py" \
	--warmup-frames="$WARMUP_FRAMES" "${RUN_DIRS[@]}"
echo "T02 benchmark complete: $RESULTS_ROOT"
