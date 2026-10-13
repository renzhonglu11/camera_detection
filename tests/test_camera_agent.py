"""Offline tests for OAuth persistence, Agent execution, and deployment arguments."""

from __future__ import annotations

import asyncio
import io
import os
import shlex
import stat
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import AsyncMock, patch

from pydantic_ai import models
from pydantic_ai.models.test import TestModel
from pydantic_ai.providers.openai_codex import OpenAICodexCredentials

from camera_agent.entry import main
from camera_agent._internal.agent import build_agent, run_agent
from camera_agent._internal.models.auth import AuthError, JsonCredentialSource, select_auth
from camera_agent._internal.models.codex import login
from camera_agent._internal.models.validation import check_environment

PROJECT_DIR = Path(__file__).resolve().parents[1]
models.ALLOW_MODEL_REQUESTS = False
os.environ["PYDANTIC_AI_NO_BANNER"] = "1"


class CredentialTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "private/oauth.json"
        self.source = JsonCredentialSource(self.path)
        self.credentials = OpenAICodexCredentials(
            access_token="test-access-token", refresh_token="test-refresh-token", account_id="test-account"
        )

    async def test_credentials_are_persisted_with_private_permissions(self):
        await self.source.save(self.credentials)
        self.assertEqual(await self.source.load(), self.credentials)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.path.parent.stat().st_mode), 0o700)

    async def test_failed_atomic_save_preserves_previous_credentials(self):
        await self.source.save(self.credentials)
        replacement = OpenAICodexCredentials(
            access_token="new-access", refresh_token="new-refresh", account_id="test-account"
        )
        with patch("camera_agent._internal.models.auth.os.replace", side_effect=OSError("write failed")):
            with self.assertRaises(OSError):
                await self.source.save(replacement)
        self.assertEqual(await self.source.load(), self.credentials)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    async def test_invalid_credentials_error_does_not_echo_secret(self):
        self.path.parent.mkdir()
        self.path.write_text('{"access_token":"private-secret","refresh_token":42}')
        with self.assertRaises(AuthError) as error:
            await self.source.load()
        self.assertNotIn("private-secret", str(error.exception))

    async def test_stored_credentials_take_priority_over_cli(self):
        await self.source.save(self.credentials)
        with patch("camera_agent._internal.models.auth.codex_cli_auth_path", return_value=self.path):
            self.assertEqual(select_auth(self.source), "stored")
            self.assertEqual(select_auth(self.source, "cli"), "cli")

    def test_missing_oauth_does_not_fall_back_to_api_key(self):
        with patch("camera_agent._internal.models.auth.codex_cli_auth_path", return_value=self.path), \
                patch.dict(os.environ, {"OPENAI_API_KEY": "test-key"}):
            with self.assertRaises(AuthError):
                select_auth(self.source)

    def test_simultaneous_credential_use_is_rejected(self):
        with self.source.exclusive():
            with self.assertRaises(AuthError):
                with self.source.exclusive():
                    self.fail("Second user acquired the credential lock")
        with self.source.exclusive():
            pass

    async def test_login_saves_independent_credentials_without_printing_tokens(self):
        class FakeFlow:
            def authorization_url(self):
                return "https://example.invalid/authorize"

            async def exchange_code_from_callback(flow):
                return self.credentials

        urls = []
        output = io.StringIO()
        with patch("camera_agent._internal.models.codex.OpenAICodexOAuthFlow", FakeFlow), redirect_stdout(output):
            path = await login(self.source, timeout=5, on_authorization_url=urls.append)
        self.assertEqual(path, self.path)
        self.assertEqual(urls, ["https://example.invalid/authorize"])
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(await self.source.load(), self.credentials)
        self.assertNotIn(self.credentials.access_token, output.getvalue())
        self.assertNotIn(self.credentials.refresh_token, output.getvalue())

    async def test_offline_check_reports_missing_login_without_network(self):
        with patch("camera_agent._internal.models.auth.codex_cli_auth_path", return_value=self.path):
            report = await check_environment(self.source)
        self.assertEqual(report["offline_agent"], "ok")
        self.assertEqual(report["oauth"], "not-configured")


class AgentTests(unittest.IsolatedAsyncioTestCase):
    async def test_agent_calls_runtime_tool_and_returns_output(self):
        agent = build_agent(TestModel(custom_output_text="ready"))
        async with asyncio.timeout(5):
            result = await agent.run("Inspect runtime.")
        self.assertEqual(result.output, "ready")
        tool_results = [
            part for message in result.all_messages() for part in message.parts
            if getattr(part, "part_kind", None) == "tool-return"
        ]
        self.assertEqual(len(tool_results), 1)
        self.assertEqual(tool_results[0].tool_name, "get_runtime_info")
        self.assertIn("architecture", tool_results[0].content)

    async def test_execution_returns_text_without_terminal_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = JsonCredentialSource(Path(directory) / "oauth.json")
            await source.save(OpenAICodexCredentials(
                access_token="test-access", refresh_token="test-refresh", account_id="test-account"
            ))
            output = io.StringIO()
            with patch("camera_agent._internal.agent.create_model", return_value=TestModel(custom_output_text="ready")), \
                    redirect_stdout(output):
                result = await run_agent(
                    source, prompt="Inspect runtime.", model_name="test", auth_mode="stored", timeout=5
                )
            self.assertEqual(result, "ready")
            self.assertEqual(output.getvalue(), "")


class EntryTests(unittest.TestCase):
    def test_run_dispatches_typed_arguments_and_formats_output(self):
        output = io.StringIO()
        handler = AsyncMock(return_value="model reply")
        with patch("camera_agent.entry.run_agent", handler), redirect_stdout(output):
            code = main([
                "--credentials-file", "/tmp/test-oauth.json", "run", "hello",
                "--model", "chosen-model", "--auth", "stored", "--timeout", "5",
            ])
        self.assertEqual(code, 0)
        self.assertEqual(output.getvalue(), "model reply\n")
        args, kwargs = handler.await_args
        self.assertEqual(args[0].path, Path("/tmp/test-oauth.json"))
        self.assertEqual(kwargs, {
            "prompt": "hello", "model_name": "chosen-model", "auth_mode": "stored", "timeout": 5,
        })

    def test_authentication_error_produces_exit_code_without_traceback(self):
        error = io.StringIO()
        handler = AsyncMock(side_effect=AuthError("No OAuth credentials found."))
        with patch("camera_agent.entry.auth_status", handler), redirect_stderr(error):
            code = main(["auth-status"])
        self.assertEqual(code, 1)
        self.assertEqual(error.getvalue(), "camera-agent: No OAuth credentials found.\n")


class DeploymentTests(unittest.TestCase):
    def test_prompt_survives_remote_shell_quoting_and_secrets_are_excluded(self):
        prompt = "don't expand $(touch /tmp/unwanted) or `commands`; keep spaces"
        result = subprocess.run(
            [str(PROJECT_DIR / "scripts/deploy_agent.sh"), "mypi", "--dry-run", "--run", "--", prompt],
            text=True, capture_output=True, check=True,
        )
        run_line = next(line for line in result.stdout.splitlines() if line.startswith("Run: "))
        command = shlex.split(run_line.removeprefix("Run: "))
        self.assertEqual(command[-2:], ["run", prompt])
        upload_line = next(line for line in result.stdout.splitlines() if line.startswith("Upload: "))
        upload = shlex.split(upload_line.removeprefix("Upload: "))
        for excluded in ("--exclude=.venv/", "--exclude=.env*", "--exclude=.state/", "--exclude=codex-oauth.json"):
            self.assertIn(excluded, upload)
        source_line = next(line for line in result.stdout.splitlines() if line.startswith("Source sync: "))
        source_sync = shlex.split(source_line.removeprefix("Source sync: "))
        self.assertIn("--delete-delay", source_sync)
        self.assertTrue(source_sync[-2].endswith("/piAgent/src/"))
        self.assertTrue(source_sync[-1].endswith("/piAgent/src/"))
        self.assertNotIn("--delete-delay", upload)


if __name__ == "__main__":
    unittest.main()
