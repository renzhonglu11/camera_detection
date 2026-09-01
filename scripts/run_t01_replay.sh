#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BIN="$PROJECT_DIR/build-cross/camera_detection"
DEFAULT_INPUT="$PROJECT_DIR/test_data/blue_ball_basic.mp4"
DEFAULT_REPLAY="unpaced"
DEFAULT_RUNS=3

BIN="$DEFAULT_BIN"
INPUT="$DEFAULT_INPUT"
REPLAY="$DEFAULT_REPLAY"
RUNS="$DEFAULT_RUNS"
EXTRA_ARGS=()

usage() {
	cat <<'EOF'
Usage: scripts/run_t01_replay.sh [options]

Run offline replay repeatedly for T01 verification.

Options:
  --bin=PATH          camera_detection binary path (default: ./build-cross/camera_detection)
  --input=PATH        input video path
  --replay=unpaced|realtime
                      replay mode (default: unpaced)
  --runs=N            number of repeated runs (default: 3)
  -- [extra args]     passthrough args appended to camera_detection
  -h, --help          show this help
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--bin=*)
			BIN="${1#*=}"
			shift
			;;
		--input=*)
			INPUT="${1#*=}"
			shift
			;;
		--replay=*)
			REPLAY="${1#*=}"
			if [[ "$REPLAY" != "unpaced" && "$REPLAY" != "realtime" ]]; then
				echo "Unsupported replay mode: $REPLAY" >&2
				exit 1
			fi
			shift
			;;
		--runs=*)
			RUNS="${1#*=}"
			if ! [[ "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
				echo "Invalid --runs value: $RUNS" >&2
				exit 1
			fi
			shift
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
done

if [[ ! -x "$BIN" ]]; then
	echo "Binary not found or not executable: $BIN" >&2
	exit 1
fi

if [[ ! -f "$INPUT" ]]; then
	echo "Input file not found: $INPUT" >&2
	exit 1
fi

echo "Using binary: $BIN"
echo "Input file: $INPUT"
echo "Replay mode: $REPLAY"
echo "Runs: $RUNS"

EXPECTED=""
for ((run=1; run<=RUNS; run++)); do
	echo "Run $run/$RUNS start"
	OUTPUT=$("$BIN" --input="$INPUT" --replay="$REPLAY" --no-display --no-stream "${EXTRA_ARGS[@]}" 2>&1 || true)
	echo "$OUTPUT"

	COMPLETED_LINE=$(echo "$OUTPUT" | grep "offline_replay_complete")
	if [[ -z "$COMPLETED_LINE" ]]; then
		echo "Run $run did not print offline_replay_complete" >&2
		exit 1
	fi

	FRAMES=$(echo "$COMPLETED_LINE" | sed -n 's/.*frames=\([0-9]\+\).*/\1/p')
	if [[ -z "$FRAMES" ]]; then
		echo "Unable to parse frame count from: $COMPLETED_LINE" >&2
		exit 1
	fi

	if [[ -z "$EXPECTED" ]]; then
		EXPECTED="$FRAMES"
	else
		if [[ "$FRAMES" != "$EXPECTED" ]]; then
			echo "Frame count mismatch: expected $EXPECTED, got $FRAMES (run $run)" >&2
			exit 1
		fi
	fi

	echo "Run $run complete: $FRAMES frames"
	echo "---"
done

echo "T01 replay verification passed: $EXPECTED frames per run"
