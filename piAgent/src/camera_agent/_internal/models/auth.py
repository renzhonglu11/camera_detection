"""Internal credential storage and source selection for model backends."""

from __future__ import annotations

import fcntl
import json
import os
import tempfile
from contextlib import contextmanager
from dataclasses import asdict
from pathlib import Path
from typing import Iterator, Literal

from pydantic_ai.providers.openai_codex import OpenAICodexCredentials

AuthMode = Literal["auto", "stored", "cli"]
CredentialMode = Literal["stored", "cli"]


class AuthError(ValueError):
    """A credential configuration problem that can be shown without token values."""


def default_credentials_path() -> Path:
    override = os.environ.get("CAMERA_AGENT_CREDENTIALS_FILE")
    if override:
        return Path(override).expanduser()
    state_dir = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
    return state_dir / "camera-detection-agent/codex-oauth.json"


def codex_cli_auth_path() -> Path:
    return Path(os.environ.get("CODEX_HOME") or Path.home() / ".codex") / "auth.json"


class JsonCredentialSource:
    """Persist refreshed tokens atomically with owner-only file permissions."""

    def __init__(self, path: Path):
        self.path = path

    async def load(self) -> OpenAICodexCredentials:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            fields = ("access_token", "refresh_token", "account_id")
            if not isinstance(data, dict) or any(
                not isinstance(data.get(key), str) or not data[key] for key in fields
            ):
                raise ValueError("invalid credential fields")
            return OpenAICodexCredentials(**{key: data[key] for key in fields})
        except (OSError, ValueError):
            raise AuthError(
                f"Cannot load OAuth credentials from {self.path}. Run camera-agent login."
            ) from None

    async def save(self, credentials: OpenAICodexCredentials) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor, temporary_path = tempfile.mkstemp(
            prefix=f".{self.path.name}.", dir=self.path.parent
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(asdict(credentials), stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary_path, self.path)
        finally:
            Path(temporary_path).unlink(missing_ok=True)

    @contextmanager
    def exclusive(self) -> Iterator[None]:
        """Keep two Agent processes from spending the same single-use refresh token."""
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor = os.open(
            self.path.with_suffix(".lock"), os.O_CREAT | os.O_RDWR, 0o600
        )
        with os.fdopen(descriptor, "w") as stream:
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise AuthError("Another camera-agent process is using these credentials.") from None
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)


def select_auth(source: JsonCredentialSource, mode: AuthMode = "auto") -> CredentialMode:
    if mode not in ("auto", "stored", "cli"):
        raise AuthError("Authentication must be auto, stored, or cli.")
    if mode in ("auto", "stored") and source.path.is_file():
        return "stored"
    if mode in ("auto", "cli") and codex_cli_auth_path().is_file():
        return "cli"
    raise AuthError("No Codex OAuth credentials found. Run camera-agent login.")
