from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.cli import (
    AGENT_SUCCESS_EXIT_GRACE_SECONDS,
    _acquire_agent_challenge,
    _deploy,
    build_parser,
)
from vitadevdeploy.errors import DeploymentError
from vitadevdeploy.protocol import Challenge


class FakeFtp:
    def __init__(self, challenge: Challenge | Exception) -> None:
        self.challenge = challenge
        self.events: list[object] = []

    def read_challenge(self) -> Challenge:
        self.events.append("read")
        if isinstance(self.challenge, Exception):
            raise self.challenge
        return self.challenge

    def wait_for_challenge(self, *, previous_nonce: str | None, timeout: float) -> Challenge:
        self.events.append(("wait", previous_nonce, timeout))
        return Challenge("cd" * 32)


class FakeDeployFtp(FakeFtp):
    def __enter__(self) -> "FakeDeployFtp":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def stage_job(self, job: object) -> str:
        self.events.append(("stage", job))
        return "/ux0:/data/VitaDevDeploy/inbox/" + job.request.job  # type: ignore[attr-defined]

    def poll_result(self, job_id: str, *, timeout: float) -> object:
        self.events.append(("result", job_id, timeout))
        return SimpleNamespace(
            title_id="TEST00001",
            succeeded=True,
            state="success",
            stage="complete",
            code=0,
            message="verified",
        )


class FakeCompanion:
    def __init__(self) -> None:
        self.events: list[object] = []

    def version(self) -> str:
        self.events.append("version")
        return "Vita Companion 1.06"

    def launch(self, title_id: str) -> str:
        self.events.append(("launch", title_id))
        return "Launched."


class CliLifecycleTests(unittest.TestCase):
    def test_parser_exposes_explicit_running_agent_mode(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--reuse-running-agent",
            ]
        )
        self.assertTrue(args.reuse_running_agent)

    def test_reuse_reads_current_challenge_without_command_port_actions(self) -> None:
        current = Challenge("ab" * 32)
        ftp = FakeFtp(current)
        companion = FakeCompanion()

        challenge, version, session = _acquire_agent_challenge(
            ftp,  # type: ignore[arg-type]
            companion,  # type: ignore[arg-type]
            reuse_running_agent=True,
            installer_title="VDEVDEP01",
            network_timeout=10.0,
        )

        self.assertEqual(challenge, current)
        self.assertIsNone(version)
        self.assertEqual(session, "reused")
        self.assertEqual(ftp.events, ["read"])
        self.assertEqual(companion.events, [])

    def test_reuse_fails_closed_when_no_valid_current_challenge_exists(self) -> None:
        ftp = FakeFtp(DeploymentError("missing"))
        companion = FakeCompanion()

        with self.assertRaisesRegex(DeploymentError, "valid current challenge"):
            _acquire_agent_challenge(
                ftp,  # type: ignore[arg-type]
                companion,  # type: ignore[arg-type]
                reuse_running_agent=True,
                installer_title="VDEVDEP01",
                network_timeout=10.0,
            )

        self.assertEqual(ftp.events, ["read"])
        self.assertEqual(companion.events, [])

    def test_verify_deploy_reuses_agent_without_any_companion_command(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--output",
                "prepared",
                "--verify-only",
                "--reuse-running-agent",
            ]
        )
        ftp = FakeDeployFtp(Challenge("ab" * 32))
        companion = FakeCompanion()
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("prepared") / ("01" * 16),
        )
        inspection = SimpleNamespace(title_id="TEST00001")

        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=inspection),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=companion),
        ):
            summary = _deploy(args)

        self.assertEqual(summary["agent_session"], "reused")
        self.assertIsNone(summary["companion_version"])
        self.assertEqual(companion.events, [])
        self.assertEqual(
            ftp.events,
            ["read", ("stage", job), ("result", job.request.job, 2100.0)],
        )

    def test_install_launch_waits_for_reused_agent_exit_without_destroy(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--output",
                "prepared",
                "--action",
                "install_launch",
                "--reuse-running-agent",
            ]
        )
        ftp = FakeDeployFtp(Challenge("ab" * 32))
        companion = FakeCompanion()
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="install_launch"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("prepared") / ("01" * 16),
        )
        inspection = SimpleNamespace(title_id="TEST00001")

        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=inspection),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=companion),
            patch("vitadevdeploy.cli.time.sleep") as sleep,
        ):
            summary = _deploy(args)

        self.assertEqual(summary["launch"], "Launched.")
        self.assertEqual(companion.events, ["version", ("launch", "TEST00001")])
        sleep.assert_called_once_with(AGENT_SUCCESS_EXIT_GRACE_SECONDS)

    def test_default_mode_launches_and_waits_for_a_new_challenge(self) -> None:
        previous = Challenge("ab" * 32)
        ftp = FakeFtp(previous)
        companion = FakeCompanion()

        challenge, version, session = _acquire_agent_challenge(
            ftp,  # type: ignore[arg-type]
            companion,  # type: ignore[arg-type]
            reuse_running_agent=False,
            installer_title="VDEVDEP01",
            network_timeout=10.0,
        )

        self.assertEqual(challenge, Challenge("cd" * 32))
        self.assertEqual(version, "Vita Companion 1.06")
        self.assertEqual(session, "launched")
        self.assertEqual(ftp.events, ["read", ("wait", previous.nonce, 15.0)])
        self.assertEqual(companion.events, ["version", ("launch", "VDEVDEP01")])


if __name__ == "__main__":
    unittest.main()
