#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="/home/rz/cam_proj/PCA9685/example/camera_detection"
BIN_PATH="${PROJECT_DIR}/bin/camera_detection"
INPUT_PATH="${PROJECT_DIR}/test_data/blue_ball_basic.mp4"
REPLAY_MODE="unpaced"
RUNS=3
DELAY_MS=0

usage() {
	cat <<'EOF'
Usage: run_t01_replay_pi.sh [options]

Run the offline T01 replay check on Raspberry Pi.

Options:
  --bin=PATH          Override camera_detection binary path
                     (default: /home/rz/cam_proj/PCA9685/example/camera_detection/bin/camera_detection)
  --input=PATH        Override input video path
                     (default: /home/rz/cam_proj/PCA9685/example/camera_detection/test_data/blue_ball_basic.mp4)
  --replay=unpaced|realtime
                     Override replay mode (default: unpaced)
  --runs=N            Override repeat count (default: 3)
  --sleep-ms=N        Optional pause between runs in milliseconds (default: 0)
  -h, --help          Show this help

This script forces:
  --no-display --no-stream

Example:
  /home/rz/cam_proj/PCA9685/example/camera_detection/scripts/run_t01_replay_pi.sh --replay=realtime
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--bin=*)
			BIN_PATH="${1#*=}"
			shift
			;;
		--input=*)
			INPUT_PATH="${1#*=}"
			shift
			;;
		--replay=*)
			REPLAY_MODE="${1#*=}"
			if [[ "$REPLAY_MODE" != "unpaced" && "$REPLAY_MODE" != "realtime" ]]; then
				echo "Unsupported replay mode: $REPLAY_MODE" >&2
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
		--sleep-ms=*)
			DELAY_MS="${1#*=}"
			if ! [[ "$DELAY_MS" =~ ^[0-9]+$ ]]; then
				echo "Invalid --sleep-ms value: $DELAY_MS" >&2
				exit 1
			fi
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage
			exit 1
			;;
	esac
done

if [[ ! -x "$BIN_PATH" ]]; then
	echo "Binary not found or not executable: $BIN_PATH" >&2
	exit 1
fi

if [[ ! -f "$INPUT_PATH" ]]; then
	echo "Input file not found: $INPUT_PATH" >&2
	exit 1
fi

if ! command -v awk >/dev/null 2>&1; then
	echo "awk is required but not found" >&2
	exit 1
fi

echo "Binary: $BIN_PATH"
echo "Input: $INPUT_PATH"
echo "Replay mode: $REPLAY_MODE"
echo "Runs: $RUNS"
if [[ "$DELAY_MS" -gt 0 ]]; then
	echo "Delay between runs: ${DELAY_MS}ms"
fi

EXPECTED=""
for ((run=1; run<=RUNS; run++)); do
	echo "Run $run/$RUNS start"
	OUTPUT=$("$BIN_PATH" --input="$INPUT_PATH" --replay="$REPLAY_MODE" --no-display --no-stream 2>&1 || true)
	echo "$OUTPUT"

	COMPLETED_LINE=$(echo "$OUTPUT" | awk '/offline_replay_complete/{print}')
	if [[ -z "$COMPLETED_LINE" ]]; then
		echo "Run $run did not print offline_replay_complete" >&2
		exit 1
	fi

	FRAMES=$(echo "$COMPLETED_LINE" | sed -n 's/.*frames=\([0-9]\+\).*/\1/p')
	EXPECTED_FRAMES=$(echo "$COMPLETED_LINE" | sed -n 's/.*expected_frames=\([0-9]\+\).*/\1/p')

	if [[ -z "$FRAMES" || -z "$EXPECTED_FRAMES" ]]; then
		echo "Unable to parse completion line: $COMPLETED_LINE" >&2
		exit 1
	fi

	if [[ "$FRAMES" != "$EXPECTED_FRAMES" ]]; then
		echo "Mismatch on run $run: frames=$FRAMES expected_frames=$EXPECTED_FRAMES" >&2
		exit 1
	fi

	if [[ -z "$EXPECTED" ]]; then
		EXPECTED="$FRAMES"
	else
		if [[ "$FRAMES" != "$EXPECTED" ]]; then
			echo "Frame count mismatch: first=$EXPECTED this=$FRAMES (run $run)" >&2
			exit 1
		fi
	fi

	echo "Run $run complete: $FRAMES frames"
	if [[ "$run" -lt "$RUNS" && "$DELAY_MS" -gt 0 ]]; then
		sleep "$(awk -v ms="$DELAY_MS" 'BEGIN {printf "%.3f", ms/1000.0}')"
	fi
	echo "---"
done

echo "T01 replay verification passed: $EXPECTED frames per run"
