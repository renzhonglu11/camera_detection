#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
AGENT_DIR="$PROJECT_DIR/piAgent"

usage() {
  cat <<'EOF'
Usage: deploy_agent.sh [target-host] [--bootstrap] [--dry-run]
       deploy_agent.sh [target-host] [--bootstrap] --run -- PROMPT [--model NAME]
       deploy_agent.sh [target-host] --login

Upload the Python Agent and install locked dependencies on the Raspberry Pi.

Options:
  target-host  SSH host/alias (default: PI_HOST from .env.local, or mypi)
  --bootstrap Install uv and the selected Python version if needed
  --dry-run   Show the upload and commands without connecting to the Pi
  --run       Run the Agent after deployment; pass arguments after --
  --login     Run independent OAuth login with a localhost:1455 SSH tunnel
  -h, --help  Show this help

The remote directory is REMOTE_AGENT_DIR, or a piAgent/ directory beside
REMOTE_DIR (the C++ bin/ directory). Credentials and virtual environments
are excluded from uploads. Login does not upload or synchronize code.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ -f "$PROJECT_DIR/.env.local" ]]; then
  source "$PROJECT_DIR/.env.local"
fi

TARGET="${PI_HOST:-mypi}"
TARGET_SET=0
REMOTE_DIR="${REMOTE_DIR:-/home/pi/camera_detection/bin}"
REMOTE_AGENT_DIR="${REMOTE_AGENT_DIR:-${REMOTE_DIR%/*}/piAgent}"
AGENT_UV_VERSION="${AGENT_UV_VERSION:-0.10.0}"
BOOTSTRAP=0
DRY_RUN=0
RUN_AFTER=0
LOGIN=0
REMOTE_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bootstrap) BOOTSTRAP=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --run) RUN_AFTER=1; shift ;;
    --login) LOGIN=1; shift ;;
    --) shift; REMOTE_ARGS=("$@"); break ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "Error: unknown option: $1" >&2; exit 1 ;;
    *)
      if [[ "$TARGET_SET" -ne 0 ]]; then
        echo "Error: unexpected argument: $1" >&2
        exit 1
      fi
      TARGET="$1"
      TARGET_SET=1
      shift
      ;;
  esac
done

if [[ "$REMOTE_AGENT_DIR" != /* || "$REMOTE_AGENT_DIR" == / ]]; then
  echo "Error: REMOTE_AGENT_DIR must be an absolute deployment directory." >&2
  exit 1
fi
if [[ ! "$AGENT_UV_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Error: AGENT_UV_VERSION must be a version such as 0.10.0." >&2
  exit 1
fi
if [[ "$LOGIN" -eq 1 && ( "$RUN_AFTER" -eq 1 || "$BOOTSTRAP" -eq 1 ) ]]; then
  echo "Error: --login is a separate step after deployment." >&2
  exit 1
fi
if [[ "$RUN_AFTER" -eq 1 && "${#REMOTE_ARGS[@]}" -eq 0 ]]; then
  echo "Error: --run requires a prompt after --." >&2
  exit 1
fi
if [[ "$RUN_AFTER" -eq 0 && "${#REMOTE_ARGS[@]}" -ne 0 ]]; then
  echo "Error: arguments after -- require --run." >&2
  exit 1
fi

quote_args() {
  local argument quoted
  for argument in "$@"; do
    printf -v quoted '%q' "$argument"
    printf '%s ' "$quoted"
  done
}

REMOTE_EXECUTABLE="$REMOTE_AGENT_DIR/.venv/bin/camera-agent"
if [[ "$LOGIN" -eq 1 ]]; then
  LOGIN_COMMAND="$(quote_args "$REMOTE_EXECUTABLE" login --no-browser)"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf 'OAuth login: ssh -L 127.0.0.1:1455:127.0.0.1:1455 %q %s\n' "$TARGET" "$LOGIN_COMMAND"
  else
    echo "Open the displayed OAuth URL in a browser on this computer."
    ssh -o ExitOnForwardFailure=yes -L 127.0.0.1:1455:127.0.0.1:1455 \
      "$TARGET" "$LOGIN_COMMAND"
  fi
  exit 0
fi

for file in pyproject.toml uv.lock .python-version; do
  if [[ ! -f "$AGENT_DIR/$file" ]]; then
    echo "Error: missing Agent project file: $AGENT_DIR/$file" >&2
    exit 1
  fi
done

UPLOAD_ARGS=(
  -az --protect-args
  --exclude=.venv/ --exclude=__pycache__/ --exclude='*.pyc'
  --exclude='.env*' --exclude=.state/ --exclude=codex-oauth.json
  --exclude=.pytest_cache/ --exclude=.ruff_cache/ --exclude=dist/
  --exclude='*.egg-info/'
)
SOURCE_SYNC_ARGS=("${UPLOAD_ARGS[@]}" --delete-delay)
UPLOAD_ARGS+=(--exclude=/src/)

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf 'Upload: '
  quote_args rsync "${UPLOAD_ARGS[@]}" "$AGENT_DIR/" "$TARGET:$REMOTE_AGENT_DIR/"
  printf '\nSource sync: '
  quote_args rsync "${SOURCE_SYNC_ARGS[@]}" "$AGENT_DIR/src/" "$TARGET:$REMOTE_AGENT_DIR/src/"
  printf '\nRemote directory: %s\n' "$REMOTE_AGENT_DIR"
  if [[ "$BOOTSTRAP" -eq 1 ]]; then
    printf 'Bootstrap uv %s and Python %s\n' "$AGENT_UV_VERSION" "$(cat "$AGENT_DIR/.python-version")"
  fi
  printf 'Sync: uv sync --locked --no-dev\nCheck: %s check\n' "$REMOTE_EXECUTABLE"
else
  ssh "$TARGET" "$(quote_args mkdir -p -- "$REMOTE_AGENT_DIR")"
  rsync "${UPLOAD_ARGS[@]}" "$AGENT_DIR/" "$TARGET:$REMOTE_AGENT_DIR/"
  # src/ is managed application code. Remove obsolete modules after upload so
  # refactors cannot leave an old entry point behind; runtime state lives outside it.
  rsync "${SOURCE_SYNC_ARGS[@]}" "$AGENT_DIR/src/" "$TARGET:$REMOTE_AGENT_DIR/src/"
  ssh "$TARGET" "$(quote_args bash -s -- "$REMOTE_AGENT_DIR" "$BOOTSTRAP" "$AGENT_UV_VERSION")" <<'REMOTE'
set -euo pipefail
agent_dir="$1"
bootstrap="$2"
uv_version="$3"
export PATH="$HOME/.local/bin:$PATH"
cd "$agent_dir"

if ! command -v uv >/dev/null 2>&1; then
  if [[ "$bootstrap" != 1 ]]; then
    echo "uv is not installed. Rerun deployment with --bootstrap." >&2
    exit 1
  fi
  curl -LsSf "https://astral.sh/uv/$uv_version/install.sh" | env UV_NO_MODIFY_PATH=1 sh
fi
if [[ "$bootstrap" == 1 ]]; then
  uv python install "$(cat .python-version)"
fi
uv sync --locked --no-dev
.venv/bin/camera-agent check
REMOTE
fi

if [[ "$RUN_AFTER" -eq 1 ]]; then
  RUN_COMMAND="$(quote_args "$REMOTE_EXECUTABLE" run "${REMOTE_ARGS[@]}")"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf 'Run: ssh %q %s\n' "$TARGET" "$RUN_COMMAND"
  else
    ssh "$TARGET" "$RUN_COMMAND"
  fi
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf 'Deployment preview complete.\n'
else
  printf 'Agent deployed to %s:%s\n' "$TARGET" "$REMOTE_AGENT_DIR"
fi
