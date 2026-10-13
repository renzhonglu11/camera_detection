"""Offline model integration checks, separate from production Agent execution."""

from __future__ import annotations

from pydantic_ai.models.test import TestModel

from ..agent import build_agent, get_runtime_info
from .auth import AuthError, JsonCredentialSource, select_auth


async def check_environment(source: JsonCredentialSource) -> dict[str, str]:
    """Exercise the tool loop locally and return diagnostics for the caller."""
    agent = build_agent(TestModel(custom_output_text="ready"))
    result = await agent.run("Read the runtime information.")
    if result.output != "ready":
        raise ValueError("Offline Agent check failed.")
    try:
        mode: str = select_auth(source)
    except AuthError:
        mode = "not-configured"
    return {**await get_runtime_info(), "offline_agent": "ok", "oauth": mode}
