#!/usr/bin/env bash
set -euo pipefail

export DEBUGINFOD_URLS=

exec /usr/bin/gdb-multiarch \
  -iex "set debuginfod enabled off" \
  "$@"
