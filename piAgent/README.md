# Camera Agent

Standalone Python subproject for the Raspberry Pi camera application. PydanticAI
runs the Agent loop and uses its `OpenAICodexModel` / `OpenAICodexProvider` to
authenticate with a ChatGPT/Codex subscription. The initial tool reads runtime
information; camera snapshots and servo commands will be connected separately.

## Package structure

```text
src/camera_agent/
├── entry.py                  # Public CLI entry: arguments, dispatch, output
├── __main__.py               # python -m camera_agent delegates to entry.main
└── _internal/
    ├── agent.py              # Agent construction, tools, and execution
    └── models/
        ├── auth.py           # Credential storage and source selection
        ├── codex.py          # Codex model creation and OAuth operations
        └── validation.py     # Offline model/tool integration checks
```

The supported external entry is `camera_agent.entry.main`; both `camera-agent`
and `python -m camera_agent` use it. `_internal` modules are implementation
details. Tests may import them to verify behavior, but external callers should
not depend on their paths.

The entry handles terminal output and browser presentation. Internal functions
accept ordinary typed arguments and return results, so they can be reused by
another interface without importing `argparse` or writing to the terminal.
Production Agent execution does not import the offline validation module.

## Local setup

From the repository root:

```bash
uv sync --project piAgent --locked
uv run --project piAgent --locked camera-agent check
uv run --project piAgent --locked camera-agent auth-status
uv run --project piAgent --locked camera-agent run "Read the runtime information."
```

The subproject has its own `pyproject.toml`, `uv.lock`, `.python-version`, and
`piAgent/.venv/`. It uses Python 3.12 by default. The default model is
`gpt-6.1-sol`, matching the local Codex configuration at setup time. Choose a
model available to your account with `--model NAME` or `CAMERA_AGENT_MODEL`.
Each run keeps only that prompt's conversation in memory.

For optional environment overrides:

```bash
cp piAgent/.env.example piAgent/.env.local
uv run --project piAgent --env-file piAgent/.env.local --locked camera-agent run "Hello"
```

## OAuth login

Authentication mode `auto` prefers an application-owned login. If none exists,
it can read the existing Codex CLI cache at `$CODEX_HOME/auth.json` or
`~/.codex/auth.json`. Use `--auth stored` or `--auth cli` to select a source.
It never falls back to an OpenAI API key.

For persistent application use, create an independent OAuth login:

```bash
uv run --project piAgent --locked camera-agent login
uv run --project piAgent --locked camera-agent run "Hello" --auth stored
```

The login uses PydanticAI's browser callback at `localhost:1455`. It times out
after five minutes. Use `login --no-browser` to open the printed URL manually.

Application credentials are stored outside the repository at
`$XDG_STATE_HOME/camera-detection-agent/codex-oauth.json`, or
`~/.local/state/camera-detection-agent/codex-oauth.json`. Override the path with
`CAMERA_AGENT_CREDENTIALS_FILE` or the global `--credentials-file PATH` option.
Files are written atomically with mode `0600`, and refreshed tokens are saved
after rotation. A process lock prevents concurrent Agent runs from refreshing
the same application credentials.

The CLI-cache fallback is useful for local connectivity checks. Its refreshed
tokens are retained only in memory by PydanticAI; independent login avoids
sharing single-use refresh tokens with a running Codex CLI. Do not upload the
local CLI auth cache or application credentials as part of code deployment.

See [PydanticAI Codex documentation](https://pydantic.dev/docs/ai/models/openai-codex/)
and [OpenAI authentication documentation](https://learn.chatgpt.com/docs/auth).

## Raspberry Pi deployment

Configure `.env.local` in the repository root using `.env.example`.
`REMOTE_AGENT_DIR` defaults to a `piAgent/` directory beside the C++ `bin/`
directory specified by `REMOTE_DIR`.

```bash
# Inspect the deployment before executing it.
./scripts/deploy_agent.sh mypi --bootstrap --dry-run

# First deployment: install uv, Python 3.12, and locked dependencies on the Pi.
./scripts/deploy_agent.sh mypi --bootstrap

# Subsequent deployments: upload source and synchronize locked dependencies.
./scripts/deploy_agent.sh mypi

# Create a separate OAuth grant on the Pi through an SSH localhost tunnel.
./scripts/deploy_agent.sh mypi --login

# Upload/sync and then send a real model request on the Pi.
./scripts/deploy_agent.sh mypi --run -- "Read the runtime information."
```

During remote login, open the printed URL in a browser on the computer running
the deployment command. SSH forwards local port 1455 to the Pi callback server.
The credentials remain on the Pi. Local port 1455 must be free.

Code uploads exclude virtual environments, Python caches, `.env` files, and
credential state. The managed `piAgent/src/` directory is synchronized with stale
source files removed after upload, so renamed modules do not remain on the Pi.
Files outside `src/` are preserved. `uv sync --locked --no-dev`
creates the target's ARM environment and checks that the lockfile matches the
project. The deployment runs an offline Agent check; model access is verified
by an explicit `--run` command after login. Bootstrap installs uv in the remote
user's executable directory and Python through uv, without sudo.

Deploy the C++ artifacts and Agent together with:

```bash
./scripts/deploy_run_camera.sh mypi --with-agent
./scripts/deploy_run_camera.sh mypi --bootstrap-agent
```

The second command includes Agent deployment and bootstraps its environment.
Neither command starts hardware control unless the C++ `--run` flag is supplied.

## Verification

Tests live under the repository's `tests/` directory and are registered with
CTest for native builds when `piAgent/.venv/bin/python` exists:

```bash
cmake -S . -B build -DCAMERA_DETECTION_BUILD_PI_APP=OFF
ctest --test-dir build -R camera_agent_test --output-on-failure
```

Tests use `TestModel` and mock OAuth exchange. They cover token persistence,
private permissions, login output, credential-source selection, the runtime
tool loop, the CLI/service boundary, and remote command quoting without real
model requests.
