"""Codex model integration and OAuth operations without terminal dependencies."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import anyio
from pydantic_ai.models.openai_codex import OpenAICodexModel
from pydantic_ai.providers.openai_codex import OpenAICodexOAuthFlow, OpenAICodexProvider

from .auth import AuthError, CredentialMode, JsonCredentialSource, select_auth


def _make_provider(source: JsonCredentialSource, mode: CredentialMode) -> OpenAICodexProvider:
    if mode == "stored":
        return OpenAICodexProvider(credential_source=source)
    if mode == "cli":
        return OpenAICodexProvider()
    raise AuthError("Resolved credential source must be stored or cli.")


def create_model(
    source: JsonCredentialSource, mode: CredentialMode, model_name: str
) -> OpenAICodexModel:
    """Create the configured model while keeping provider details in this module."""
    model_name = model_name.removeprefix("openai-codex:").strip()
    if not model_name or ":" in model_name:
        raise ValueError("Use a Codex model name, such as gpt-6.1-sol.")
    return OpenAICodexModel(model_name, provider=_make_provider(source, mode))


async def auth_status(source: JsonCredentialSource) -> CredentialMode:
    """Validate the cached credential format without making a model request."""
    mode = select_auth(source)
    if mode == "stored":
        await source.load()
    else:
        async with _make_provider(source, mode):
            pass
    return mode


async def login(
    source: JsonCredentialSource,
    *,
    on_authorization_url: Callable[[str], None],
    timeout: int = 300,
) -> Path:
    """Request an independent OAuth grant and return its persisted file path.

    The caller presents the authorization URL in its own interface. This module
    owns token exchange and persistence, and never prints credentials or URLs.
    """
    if timeout <= 0:
        raise ValueError("Login timeout must be positive.")
    with source.exclusive():
        flow = OpenAICodexOAuthFlow()
        on_authorization_url(flow.authorization_url())
        with anyio.fail_after(timeout):
            credentials = await flow.exchange_code_from_callback()
        await source.save(credentials)
    return source.path
