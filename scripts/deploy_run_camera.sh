#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-cross"
LOCAL_BIN="$BUILD_DIR/camera_detection"
LOCAL_FRONTEND="$BUILD_DIR/index.html"

# Load configuration from .env.local (user-specific, not committed to git)
ENV_FILE="$PROJECT_DIR/.env.local"
if [[ ! -f "$ENV_FILE" ]]; then
  echo "Error: Configuration file not found at '$ENV_FILE'" >&2
  echo "Create it by copying .env.example: cp $PROJECT_DIR/.env.example $ENV_FILE" >&2
  exit 1
fi
source "$ENV_FILE"

# Set defaults for optional config values
REMOTE_DIR="${REMOTE_DIR:-/home/pi/camera_detection/bin}"
RUN_AFTER="${RUN_AFTER:-0}"
USE_SUDO="${USE_SUDO:-0}"

REMOTE_BIN="$REMOTE_DIR/camera_detection"
REMOTE_FRONTEND="$REMOTE_DIR/index.html"

usage() {
  cat <<EOF
Usage: $(basename "$0") [target-host] [--run] [--sudo]
       $(basename "$0") [target-host] --run -- [camera_detection args...]

Deploys build-cross/camera_detection to:
  $REMOTE_DIR

Options:
  target-host  SSH host/alias (default: mypi, or PI_HOST env var)
  --run        Run the binary on the Pi after deploy
  --sudo       Run with sudo (only applies with --run)
  -h, --help   Show this help

Examples:
  $(basename "$0")
  $(basename "$0") mypi --run
  $(basename "$0") pi@raspberrypi.local --run --sudo
  $(basename "$0") mypi --run -- --pan-tracking --pan-test-sweep --no-display
EOF
}

TARGET="${PI_HOST:-mypi}"
TARGET_SET=0
RUN_AFTER=0
USE_SUDO=0
REMOTE_ARGS=()

CONTROL_PATH="/tmp/deploy_run_camera_%r_%h_%p"
SSH_COMMON_OPTS=(
  -o ControlMaster=auto
  -o ControlPersist=2m
  -o "ControlPath=$CONTROL_PATH"
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --)
      shift
      REMOTE_ARGS=("$@")
      break
      ;;
    --run)
      RUN_AFTER=1
      shift
      ;;
    --sudo)
      USE_SUDO=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      if [[ "$TARGET_SET" -eq 0 ]]; then
        TARGET="$1"
        TARGET_SET=1
        shift
      else
        echo "Error: unexpected argument '$1'" >&2
        usage
        exit 1
      fi
      ;;
  esac
done

if [[ ! -f "$LOCAL_BIN" ]]; then
  echo "Error: binary not found at '$LOCAL_BIN'" >&2
  echo "Build first with: cmake -S $PROJECT_DIR -B $BUILD_DIR -DCMAKE_TOOLCHAIN_FILE=$PROJECT_DIR/toolchain.cmake && cmake --build $BUILD_DIR -j" >&2
  exit 1
fi

if [[ ! -f "$LOCAL_FRONTEND" ]]; then
  echo "Error: frontend asset not found at '$LOCAL_FRONTEND'" >&2
  echo "Build first with: cmake -S $PROJECT_DIR -B $BUILD_DIR -DCMAKE_TOOLCHAIN_FILE=$PROJECT_DIR/toolchain.cmake && cmake --build $BUILD_DIR -j" >&2
  exit 1
fi

echo "Creating remote folder: $TARGET:$REMOTE_DIR"
ssh "${SSH_COMMON_OPTS[@]}" "$TARGET" "mkdir -p '$REMOTE_DIR'"

echo "Deploying binary to Pi..."
rsync -avz --progress -e "ssh -o ControlMaster=auto -o ControlPersist=2m -o ControlPath=$CONTROL_PATH" "$LOCAL_BIN" "$TARGET:$REMOTE_DIR/"

echo "Deploying frontend to Pi..."
rsync -avz --progress -e "ssh -o ControlMaster=auto -o ControlPersist=2m -o ControlPath=$CONTROL_PATH" "$LOCAL_FRONTEND" "$TARGET:$REMOTE_DIR/"

echo "Setting execute permission on Pi..."
ssh "${SSH_COMMON_OPTS[@]}" "$TARGET" "chmod +x '$REMOTE_BIN'"

if [[ "$RUN_AFTER" -eq 1 ]]; then
  REMOTE_COMMAND=("'$REMOTE_BIN'")
  if [[ "${#REMOTE_ARGS[@]}" -gt 0 ]]; then
    printf -v REMOTE_ARG_STRING ' %q' "${REMOTE_ARGS[@]}"
    REMOTE_COMMAND+=("$REMOTE_ARG_STRING")
  fi

  if [[ "$USE_SUDO" -eq 1 ]]; then
    echo "Running with sudo on Pi..."
    ssh -t "${SSH_COMMON_OPTS[@]}" "$TARGET" "sudo ${REMOTE_COMMAND[*]}"
  else
    echo "Running on Pi..."
    ssh -t "${SSH_COMMON_OPTS[@]}" "$TARGET" "${REMOTE_COMMAND[*]}"
  fi
fi

echo "Done. Deployed to: $TARGET:$REMOTE_BIN"
echo "Frontend asset: $TARGET:$REMOTE_FRONTEND"
