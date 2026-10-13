"""Agent construction, runtime tools, and execution independent of the CLI."""

from __future__ import annotations

import asyncio
import platform
from contextlib import nullcontext
from importlib.metadata import version

from pydantic_ai import Agent
from pydantic_ai.models import Model
from pydantic_ai.usage import UsageLimits

from .models.auth import AuthMode, JsonCredentialSource, select_auth
from .models.codex import create_model


async def get_runtime_info() -> dict[str, str]:
    """Read the operating system, processor architecture, and Python Agent versions."""
    return {
        "system": platform.system(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "pydantic_ai": version("pydantic-ai-slim"),
    }


def build_agent(model: Model) -> Agent:
    agent = Agent(
        model,
        name="camera_detection_agent",
        instructions=(
            "You assist with a Raspberry Pi camera and servo application. "
            "Reply in the user's language. Use get_runtime_info for runtime facts. "
            "Camera snapshots and servo control tools have not been connected yet. "
            "Do not claim to observe images or move hardware."
        ),
    )
    agent.tool_plain(get_runtime_info)
    return agent


async def run_agent(
    source: JsonCredentialSource,
    *,
    prompt: str,
    model_name: str,
    auth_mode: AuthMode = "auto",
    timeout: float = 90,
) -> str:
    """Run one conversation and return its final text without terminal output."""
    if timeout <= 0:
        raise ValueError("Request timeout must be positive.")
    mode = select_auth(source, auth_mode)
    with source.exclusive() if mode == "stored" else nullcontext():
        agent = build_agent(create_model(source, mode, model_name))
        async with asyncio.timeout(timeout), agent:
            result = await agent.run(prompt, usage_limits=UsageLimits(request_limit=8))
    return result.output
