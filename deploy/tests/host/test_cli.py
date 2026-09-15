from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.cli import (
    AGENT_SUCCESS_EXIT_GRACE_SECONDS,
    _acquire_agent_challenge,
    _deploy,
    _resume_direct,
    build_parser,
)
from vitadevdeploy.errors import DeploymentError
from vitadevdeploy.direct import DirectCommitState, DirectTransferError
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
    existing_result: object | None = None

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

    def read_result(self, job_id: str) -> object | None:
        self.events.append(("read_result", job_id))
        return self.existing_result


class FakeCompanion:
    def __init__(self) -> None:
        self.events: list[object] = []

    def version(self) -> str:
        self.events.append("version")
        return "Vita Companion 1.06"

    def launch(self, title_id: str) -> str:
        self.events.append(("launch", title_id))
        return "Launched."


class FakeDirect:
    def __init__(self) -> None:
        self.challenge: Challenge | None = None
        self.events: list[object] = []

    def connect_wait(self, *, timeout: float) -> "FakeDirect":
        self.events.append(("connect", timeout))
        self.challenge = Challenge("ab" * 32)
        return self

    def stage_job(self, job_or_plan: object) -> object:
        job = getattr(job_or_plan, "job", job_or_plan)
        self.events.append(("stage", job))
        return SimpleNamespace(
            resumed_files=0,
            resumed_bytes=0,
            committed_files=job.manifest.file_count,
            committed_bytes=job.manifest.total_size,
            commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
        )

    def close(self) -> None:
        self.events.append("close")


class CliLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.direct_plan_patch = patch(
            "vitadevdeploy.cli.prepare_direct_transfer",
            side_effect=lambda job: job,
        )
        self.direct_plan_patch.start()

    def tearDown(self) -> None:
        self.direct_plan_patch.stop()

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

    def test_parser_exposes_direct_tcp_without_changing_ftp_default(self) -> None:
        common = [
            "deploy",
            "test.vpk",
            "--vita",
            "192.0.2.1",
            "--private-key",
            "key.pem",
        ]
        self.assertEqual(build_parser().parse_args(common).transport, "ftp")
        direct = build_parser().parse_args(common + ["--transport", "tcp", "--tcp-port", "19000"])
        self.assertEqual(direct.transport, "tcp")
        self.assertEqual(direct.tcp_port, 19000)

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

    def test_direct_transport_streams_job_then_uses_ftp_only_for_result(self) -> None:
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
                "--transport",
                "tcp",
            ]
        )
        result_ftp = FakeDeployFtp(Challenge("ff" * 32))
        direct = FakeDirect()
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
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=direct),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=result_ftp),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=companion),
        ):
            summary = _deploy(args)

        self.assertEqual(summary["transport"], "tcp")
        self.assertEqual(summary["direct_committed_files"], 2)
        self.assertEqual(summary["direct_committed_bytes"], 100)
        self.assertEqual(
            summary["direct_commit_state"],
            DirectCommitState.KNOWN_COMMITTED.value,
        )
        self.assertFalse(summary["direct_recovered_after_ambiguous_commit"])
        self.assertEqual(companion.events, [])
        self.assertEqual(
            direct.events,
            [("connect", 15.0), "close", ("connect", 15.0), ("stage", job), "close"],
        )
        self.assertEqual(
            result_ftp.events,
            [("result", job.request.job, 2100.0)],
        )

    def test_direct_preparation_runs_between_challenge_probe_and_transfer_connection(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--output", "prepared",
                "--verify-only", "--reuse-running-agent", "--transport", "tcp",
            ]
        )
        events: list[str] = []

        class OrderedDirect(FakeDirect):
            def connect_wait(self, *, timeout: float) -> "FakeDirect":
                events.append("connect")
                return super().connect_wait(timeout=timeout)

            def close(self) -> None:
                events.append("close")
                super().close()

            def stage_job(self, job_or_plan: object) -> object:
                events.append("stage")
                return super().stage_job(job_or_plan)

        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("prepared") / ("01" * 16),
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch(
                "vitadevdeploy.cli._prepare",
                side_effect=lambda *args, **kwargs: events.append("prepare") or job,
            ),
            patch(
                "vitadevdeploy.cli.prepare_direct_transfer",
                side_effect=lambda value: events.append("plan") or value,
            ),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=OrderedDirect()),
            patch(
                "vitadevdeploy.cli.VitaFtpClient",
                return_value=FakeDeployFtp(Challenge("ff" * 32)),
            ),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
        ):
            _deploy(args)

        self.assertEqual(
            events,
            ["connect", "close", "prepare", "plan", "connect", "stage", "close"],
        )

    def test_resume_direct_reuses_retained_signature_and_same_live_challenge(self) -> None:
        args = build_parser().parse_args(
            [
                "resume-direct",
                "retained-job",
                "--vita",
                "192.0.2.1",
            ]
        )
        ftp = FakeDeployFtp(Challenge("ff" * 32))
        direct = FakeDirect()
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("retained-job"),
        )

        with (
            patch("vitadevdeploy.cli.load_prepared_job", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=direct),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
        ):
            summary = _resume_direct(args)

        self.assertEqual(summary["mode"], "resume-direct")
        self.assertFalse(summary["existing_result"])
        self.assertEqual(direct.events, [("connect", 10.0), ("stage", job), "close"])
        self.assertEqual(
            ftp.events,
            [
                ("read_result", job.request.job),
                ("result", job.request.job, 2100.0),
            ],
        )

    def test_resume_direct_uses_existing_terminal_result_without_restreaming(self) -> None:
        args = build_parser().parse_args(
            ["resume-direct", "retained-job", "--vita", "192.0.2.1"]
        )
        ftp = FakeDeployFtp(Challenge("ff" * 32))
        ftp.existing_result = SimpleNamespace(
            title_id="TEST00001",
            succeeded=True,
            state="success",
            stage="complete",
            code=0,
            message="verified",
        )
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("retained-job"),
        )

        with (
            patch("vitadevdeploy.cli.load_prepared_job", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient") as direct_type,
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
            patch("vitadevdeploy.cli.prepare_direct_transfer") as prepare_plan,
        ):
            summary = _resume_direct(args)

        self.assertTrue(summary["existing_result"])
        direct_type.assert_not_called()
        prepare_plan.assert_not_called()

    def test_resume_direct_rechecks_result_after_lost_commit_ack(self) -> None:
        args = build_parser().parse_args(
            ["resume-direct", "retained-job", "--vita", "192.0.2.1"]
        )
        terminal = SimpleNamespace(
            title_id="TEST00001",
            succeeded=True,
            state="success",
            stage="complete",
            code=0,
            message="verified",
        )

        class SequencedFtp(FakeDeployFtp):
            def __init__(self) -> None:
                super().__init__(Challenge("ff" * 32))
                self.reads = 0
                self.polls = 0

            def read_result(self, job_id: str) -> object | None:
                self.events.append(("read_result", job_id))
                self.reads += 1
                return None

            def poll_result(self, job_id: str, *, timeout: float) -> object:
                self.events.append(("result", job_id, timeout))
                self.polls += 1
                return terminal

        class FailedReconnect(FakeDirect):
            def connect_wait(self, *, timeout: float) -> "FakeDirect":
                self.events.append(("connect", timeout))
                raise DeploymentError("listener already left the intake loop")

        ftp = SequencedFtp()
        direct = FailedReconnect()
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=Path("retained-job"),
        )
        with (
            patch("vitadevdeploy.cli.load_prepared_job", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=direct),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
        ):
            summary = _resume_direct(args)

        self.assertFalse(summary["existing_result"])
        self.assertTrue(summary["direct_recovered_after_ambiguous_commit"])
        self.assertEqual(direct.events, [("connect", 10.0), "close"])
        self.assertEqual(ftp.reads, 1)
        self.assertEqual(ftp.polls, 1)

    def test_direct_deploy_recovers_delayed_result_after_ambiguous_ack(self) -> None:
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
                "--transport",
                "tcp",
            ]
        )

        class AmbiguousDirect(FakeDirect):
            def stage_job(self, job: object) -> object:
                self.events.append(("stage", job))
                raise DirectTransferError(
                    "final acknowledgement was lost",
                    commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                    job=job.request.job,
                    result_may_appear=True,
                )

        ftp = FakeDeployFtp(Challenge("ff" * 32))
        direct = AmbiguousDirect()
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
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=direct),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=ftp),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=companion),
        ):
            summary = _deploy(args)

        self.assertEqual(
            summary["direct_commit_state"],
            DirectCommitState.KNOWN_COMMITTED.value,
        )
        self.assertTrue(summary["direct_recovered_after_ambiguous_commit"])
        self.assertEqual(ftp.events, [("result", job.request.job, 2100.0)])

    def test_direct_title_mismatch_never_advances_or_discards_evidence(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--verify-only",
                "--reuse-running-agent", "--transport", "tcp",
            ]
        )

        class MismatchedResultFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                return SimpleNamespace(
                    title_id="OTHER0001", succeeded=True, state="success",
                    stage="complete", code=0, message="verified",
                )

        output = Path("C:/temporary/vitadevdeploy-job-title-mismatch")
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("01" * 16),
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=FakeDirect()),
            patch(
                "vitadevdeploy.cli.VitaFtpClient",
                return_value=MismatchedResultFtp(Challenge("ff" * 32)),
            ),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaisesRegex(DirectTransferError, "TITLE_ID") as captured:
                _deploy(args)

        self.assertEqual(
            captured.exception.commit_state,
            DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
        )
        remove_tree.assert_not_called()

    def test_direct_deploy_retains_temporary_job_when_result_stays_unknown(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--verify-only",
                "--reuse-running-agent",
                "--transport",
                "tcp",
            ]
        )

        class AmbiguousDirect(FakeDirect):
            def stage_job(self, job: object) -> object:
                raise DirectTransferError(
                    "final acknowledgement was lost",
                    commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                    job=job.request.job,
                    result_may_appear=True,
                )

        class MissingResultFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                raise DeploymentError("result still absent")

        output = Path("C:/temporary/vitadevdeploy-job-test")
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("01" * 16),
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=AmbiguousDirect()),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=MissingResultFtp(Challenge("ff" * 32))),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaisesRegex(
                DirectTransferError, "retained signed job evidence"
            ) as captured:
                _deploy(args)

        self.assertEqual(
            captured.exception.commit_state,
            DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
        )
        remove_tree.assert_not_called()

    def test_direct_deploy_retains_temporary_job_after_negative_v1_ack(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--verify-only",
                "--reuse-running-agent",
                "--transport",
                "tcp",
            ]
        )

        class RejectedDirect(FakeDirect):
            def stage_job(self, job: object) -> object:
                raise DirectTransferError(
                    "receiver rejected the signed metadata",
                    commit_state=DirectCommitState.FAIL_BEFORE_COMMIT,
                    job=job.request.job,
                )

        output = Path("C:/temporary/vitadevdeploy-job-rejected")
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("01" * 16),
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=RejectedDirect()),
            patch("vitadevdeploy.cli.VitaFtpClient") as ftp_type,
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaises(DirectTransferError) as captured:
                _deploy(args)

        self.assertEqual(
            captured.exception.commit_state,
            DirectCommitState.FAIL_BEFORE_COMMIT,
        )
        ftp_type.assert_not_called()
        remove_tree.assert_not_called()

    def test_direct_deploy_retains_job_when_v1_commit_report_loses_result(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy",
                "test.vpk",
                "--vita",
                "192.0.2.1",
                "--private-key",
                "key.pem",
                "--verify-only",
                "--reuse-running-agent",
                "--transport",
                "tcp",
            ]
        )

        class MissingResultFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                raise DeploymentError("result channel disconnected")

        output = Path("C:/temporary/vitadevdeploy-job-committed")
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("01" * 16),
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=FakeDirect()),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=MissingResultFtp(Challenge("ff" * 32))),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaisesRegex(
                DirectTransferError, "ambiguous_after_commit"
            ) as captured:
                _deploy(args)

        self.assertEqual(
            captured.exception.commit_state,
            DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
        )
        remove_tree.assert_not_called()

    def test_terminal_install_failure_with_marker_retains_signed_job(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--reuse-running-agent",
                "--transport", "tcp", "--action", "install",
            ]
        )

        class FailedInstallFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                return SimpleNamespace(
                    title_id="TEST00001", succeeded=False, state="failed",
                    stage="promote", code=-1, message="installer failed",
                )

        output = Path("C:/temporary/vitadevdeploy-job-failed-install")
        job = SimpleNamespace(
            request=SimpleNamespace(job="03" * 16, title_id="TEST00001", action="install"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("03" * 16),
        )
        snapshot = SimpleNamespace(
            safe_to_retry=False,
            disposition="failure_reported_marker_stale",
            operator_action="do not retry automatically",
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=FakeDirect()),
            patch(
                "vitadevdeploy.cli.VitaFtpClient",
                return_value=FailedInstallFtp(Challenge("ff" * 32)),
            ),
            patch(
                "vitadevdeploy.cli._reconcile_terminal_install_failure",
                return_value=snapshot,
            ) as reconcile,
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaisesRegex(
                DeploymentError, "failure_reported_marker_stale"
            ):
                _deploy(args)

        reconcile.assert_called_once_with(args, job_root=job.root)
        remove_tree.assert_not_called()

    def test_terminal_install_failure_without_marker_can_drop_derived_job(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--reuse-running-agent",
                "--transport", "tcp", "--action", "install",
            ]
        )

        class FailedInstallFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                return SimpleNamespace(
                    title_id="TEST00001", succeeded=False, state="failed",
                    stage="tree", code=-20005, message="hash mismatch",
                )

        output = Path("C:/temporary/vitadevdeploy-job-clean-failure")
        job = SimpleNamespace(
            request=SimpleNamespace(job="04" * 16, title_id="TEST00001", action="install"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("04" * 16),
        )
        snapshot = SimpleNamespace(
            safe_to_retry=True,
            disposition="clean",
            operator_action="no promotion recovery marker is present",
        )
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=FakeDirect()),
            patch(
                "vitadevdeploy.cli.VitaFtpClient",
                return_value=FailedInstallFtp(Challenge("ff" * 32)),
            ),
            patch(
                "vitadevdeploy.cli._reconcile_terminal_install_failure",
                return_value=snapshot,
            ),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
        ):
            with self.assertRaisesRegex(DeploymentError, "hash mismatch"):
                _deploy(args)

        remove_tree.assert_called_once_with(output, ignore_errors=True)

    def test_keyboard_interrupt_during_direct_stream_retains_visible_job(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--verify-only",
                "--reuse-running-agent", "--transport", "tcp",
            ]
        )

        class InterruptedDirect(FakeDirect):
            def stage_job(self, job: object) -> object:
                self.events.append(("stage", job))
                raise KeyboardInterrupt()

        output = Path("C:/temporary/vitadevdeploy-job-interrupted-stream")
        job = SimpleNamespace(
            request=SimpleNamespace(job="01" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("01" * 16),
        )
        stderr = io.StringIO()
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=InterruptedDirect()),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
            redirect_stderr(stderr),
        ):
            with self.assertRaises(KeyboardInterrupt):
                _deploy(args)

        remove_tree.assert_not_called()
        self.assertIn(job.request.job, stderr.getvalue())
        self.assertIn(str(job.root), stderr.getvalue())

    def test_keyboard_interrupt_during_known_commit_poll_retains_visible_job(self) -> None:
        args = build_parser().parse_args(
            [
                "deploy", "test.vpk", "--vita", "192.0.2.1",
                "--private-key", "key.pem", "--verify-only",
                "--reuse-running-agent", "--transport", "tcp",
            ]
        )

        class InterruptedPollFtp(FakeDeployFtp):
            def poll_result(self, job_id: str, *, timeout: float) -> object:
                raise KeyboardInterrupt()

        output = Path("C:/temporary/vitadevdeploy-job-interrupted-poll")
        job = SimpleNamespace(
            request=SimpleNamespace(job="02" * 16, title_id="TEST00001", action="verify"),
            manifest=SimpleNamespace(file_count=2, total_size=100, sha256="ef" * 32),
            root=output / ("02" * 16),
        )
        stderr = io.StringIO()
        with (
            patch("vitadevdeploy.cli.inspect_vpk", return_value=SimpleNamespace(title_id="TEST00001")),
            patch("vitadevdeploy.cli._prepare", return_value=job),
            patch("vitadevdeploy.cli.VitaDirectClient", return_value=FakeDirect()),
            patch("vitadevdeploy.cli.VitaFtpClient", return_value=InterruptedPollFtp(Challenge("ff" * 32))),
            patch("vitadevdeploy.cli.VitaCompanionClient", return_value=FakeCompanion()),
            patch("vitadevdeploy.cli.tempfile.mkdtemp", return_value=str(output)),
            patch("vitadevdeploy.cli.shutil.rmtree") as remove_tree,
            redirect_stderr(stderr),
        ):
            with self.assertRaises(KeyboardInterrupt):
                _deploy(args)

        remove_tree.assert_not_called()
        self.assertIn(job.request.job, stderr.getvalue())
        self.assertIn(str(job.root), stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
