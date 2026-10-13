"""Public command-line entry: parse arguments, dispatch, and present results."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import webbrowser
from collections.abc import Sequence
from pathlib import Path

from pydantic_ai.exceptions import ModelAPIError, UserError

from ._internal.agent import run_agent
from ._internal.models.auth import AuthError, JsonCredentialSource, default_credentials_path
from ._internal.models.codex import auth_status, login

__all__ = ["main"]


def _build_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="PydanticAI camera Agent using Codex OAuth.")
    result.add_argument(
        "--credentials-file", type=Path, default=default_credentials_path(),
        help="Application-owned OAuth credential file (kept outside the repository by default).",
    )
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="Check the environment and Agent loop without network calls.")
    commands.add_parser("auth-status", help="Show the credential source without exposing tokens.")
    login_command = commands.add_parser("login", help="Create an independent, persistent Codex OAuth login.")
    login_command.add_argument("--no-browser", action="store_true", help="Print the URL without opening a browser.")
    login_command.add_argument("--timeout", type=int, default=300, help="Login timeout in seconds (default: 300).")
    run_command = commands.add_parser("run", help="Send a prompt to the Codex model.")
    run_command.add_argument("prompt")
    run_command.add_argument("--model", default=os.environ.get("CAMERA_AGENT_MODEL", "gpt-6.1-sol"))
    run_command.add_argument("--timeout", type=float, default=90, help="Request timeout in seconds (default: 90).")
    run_command.add_argument(
        "--auth", choices=("auto", "stored", "cli"),
        default=os.environ.get("CAMERA_AGENT_AUTH", "auto"),
    )
    return result


def _present_login_url(url: str, *, open_browser: bool) -> None:
    print("Open this URL in your browser to sign in:", flush=True)
    print(url, flush=True)
    print("Waiting for the callback on localhost:1455...", flush=True)
    if open_browser:
        webbrowser.open(url)


async def _dispatch(args: argparse.Namespace) -> None:
    source = JsonCredentialSource(args.credentials_file.expanduser())
    if args.command == "check":
        # TestModel and validation helpers are loaded only for this command.
        from ._internal.models.validation import check_environment

        print(json.dumps(await check_environment(source), indent=2))
    elif args.command == "auth-status":
        print(f"Codex OAuth: {await auth_status(source)}")
    elif args.command == "login":
        path = await login(
            source,
            timeout=args.timeout,
            on_authorization_url=lambda url: _present_login_url(url, open_browser=not args.no_browser),
        )
        print(f"OAuth login saved to {path} (permissions: 0600).")
    else:
        output = await run_agent(
            source, prompt=args.prompt, model_name=args.model,
            auth_mode=args.auth, timeout=args.timeout,
        )
        print(output)


def main(argv: Sequence[str] | None = None) -> int:
    """Run the CLI, optionally with explicit arguments for embedding or tests."""
    args = _build_parser().parse_args(argv)
    os.environ.setdefault("PYDANTIC_AI_NO_BANNER", "1")
    try:
        asyncio.run(_dispatch(args))
    except (AuthError, ModelAPIError, UserError, OSError, ValueError, TimeoutError) as error:
        message = str(error) or "Operation timed out. Check connectivity or increase --timeout."
        print(f"camera-agent: {message}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0
