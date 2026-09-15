"""Command-line interface for local preparation and Vita deployment."""

from __future__ import annotations

import argparse
import json
import secrets
import shutil
import sys
import tempfile
import time
from pathlib import Path

from .companion import VitaCompanionClient
from .crypto import export_public_key, generate_keypair
from .direct import (
    DEFAULT_DIRECT_PORT,
    DirectCommitState,
    DirectTransferError,
    VitaDirectClient,
    prepare_direct_transfer,
)
from .errors import DeploymentError, VitaDevDeployError
from .ftp import DEFAULT_REMOTE_ROOT, VitaFtpClient
from .job import load_prepared_job, prepare_job, verify_prepared_job
from .protocol import DEPLOYER_TITLE_ID, Challenge, parse_challenge
from .recovery import collect_recovery_snapshot
from .vpk import VpkLimits, inspect_vpk


AGENT_SUCCESS_EXIT_GRACE_SECONDS = 4.0


def _limits(args: argparse.Namespace) -> VpkLimits:
    return VpkLimits(
        max_entries=args.max_entries,
        max_file_size=args.max_file_size,
        max_total_size=args.max_total_size,
        max_compression_ratio=args.max_compression_ratio,
    )


def _add_limits(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--max-entries", type=int, default=8192)
    parser.add_argument("--max-file-size", type=int, default=256 * 1024 * 1024)
    parser.add_argument("--max-total-size", type=int, default=1024 * 1024 * 1024)
    parser.add_argument("--max-compression-ratio", type=int, default=1000)


def _add_crypto(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--crypto-backend", choices=("auto", "cryptography", "openssl"), default="auto")
    parser.add_argument(
        "--openssl",
        help="OpenSSL executable (or set VITADEVDEPLOY_OPENSSL)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="vitadevdeploy")
    subparsers = parser.add_subparsers(dest="command", required=True)

    verify_parser = subparsers.add_parser("verify", help="validate a VPK without writing or connecting")
    verify_parser.add_argument("vpk", type=Path)
    _add_limits(verify_parser)

    key_parser = subparsers.add_parser("keygen", help="generate an Ed25519 developer key pair")
    key_parser.add_argument("--private", type=Path, required=True)
    key_parser.add_argument("--public", type=Path, required=True)
    key_parser.add_argument("--raw-public", type=Path)
    key_parser.add_argument("--c-header", type=Path)
    key_parser.add_argument("--force", action="store_true")
    _add_crypto(key_parser)

    export_parser = subparsers.add_parser("export-public", help="export a raw key and Vita C header")
    export_parser.add_argument("--public", type=Path, required=True)
    export_parser.add_argument("--raw", type=Path)
    export_parser.add_argument("--c-header", type=Path)
    export_parser.add_argument("--force", action="store_true")
    _add_crypto(export_parser)

    prepare_parser = subparsers.add_parser("prepare", help="create a signed local job directory")
    prepare_parser.add_argument("vpk", type=Path)
    prepare_parser.add_argument("--output", type=Path, required=True)
    prepare_parser.add_argument("--private-key", type=Path, required=True)
    prepare_parser.add_argument("--head-template", type=Path)
    prepare_parser.add_argument("--installer-title", default=DEPLOYER_TITLE_ID)
    nonce_group = prepare_parser.add_mutually_exclusive_group(required=True)
    nonce_group.add_argument("--nonce")
    nonce_group.add_argument("--challenge-file", type=Path)
    prepare_parser.add_argument("--job-id")
    prepare_parser.add_argument("--action", choices=("verify", "install", "install_launch"), default="install_launch")
    _add_crypto(prepare_parser)
    _add_limits(prepare_parser)

    check_parser = subparsers.add_parser("check-job", help="verify a prepared job and signature")
    check_parser.add_argument("job", type=Path)
    check_parser.add_argument("--public-key", type=Path, required=True)
    _add_crypto(check_parser)

    deploy_parser = subparsers.add_parser("deploy", help="prepare, upload, verify/install, and optionally launch")
    deploy_parser.add_argument("vpk", type=Path)
    deploy_parser.add_argument("--vita", required=True)
    deploy_parser.add_argument("--private-key", type=Path, required=True)
    deploy_parser.add_argument("--head-template", type=Path)
    deploy_parser.add_argument("--installer-title", default=DEPLOYER_TITLE_ID)
    deploy_parser.add_argument(
        "--transport",
        choices=("ftp", "tcp"),
        default="ftp",
        help=(
            "package intake transport; tcp sends signed job bytes directly "
            "to VitaDevDeploy while ftp preserves the hardware-tested path"
        ),
    )
    deploy_parser.add_argument("--ftp-port", type=int, default=1337)
    deploy_parser.add_argument("--tcp-port", type=int, default=DEFAULT_DIRECT_PORT)
    deploy_parser.add_argument("--command-port", type=int, default=1338)
    deploy_parser.add_argument("--network-timeout", type=float, default=10.0)
    deploy_parser.add_argument("--result-timeout", type=float, default=2100.0)
    deploy_parser.add_argument("--action", choices=("verify", "install", "install_launch"), default="install_launch")
    deploy_parser.add_argument("--verify-only", action="store_true", help="send action=verify and never promote or launch")
    deploy_parser.add_argument(
        "--reuse-running-agent",
        action="store_true",
        help="use the current agent challenge without closing or launching applications",
    )
    deploy_parser.add_argument("--dry-run", action="store_true", help="perform full local preparation without network access")
    deploy_parser.add_argument("--output", type=Path, help="retain the prepared job instead of using a temporary directory")
    _add_crypto(deploy_parser)
    _add_limits(deploy_parser)

    resume_parser = subparsers.add_parser(
        "resume-direct",
        help="retry one retained, already-signed direct TCP job against the same live challenge",
    )
    resume_parser.add_argument("job", type=Path)
    resume_parser.add_argument("--vita", required=True)
    resume_parser.add_argument("--tcp-port", type=int, default=DEFAULT_DIRECT_PORT)
    resume_parser.add_argument("--ftp-port", type=int, default=1337)
    resume_parser.add_argument("--command-port", type=int, default=1338)
    resume_parser.add_argument("--network-timeout", type=float, default=10.0)
    resume_parser.add_argument("--result-timeout", type=float, default=2100.0)

    recovery_parser = subparsers.add_parser(
        "recovery-status",
        help="read and classify durable install-recovery evidence without changing the Vita",
    )
    recovery_parser.add_argument("--vita", required=True)
    recovery_parser.add_argument("--ftp-port", type=int, default=1337)
    recovery_parser.add_argument("--network-timeout", type=float, default=10.0)
    return parser


def _job_summary(job: object) -> dict[str, object]:
    prepared = job
    return {
        "job": prepared.request.job,  # type: ignore[attr-defined]
        "title_id": prepared.request.title_id,  # type: ignore[attr-defined]
        "action": prepared.request.action,  # type: ignore[attr-defined]
        "file_count": prepared.manifest.file_count,  # type: ignore[attr-defined]
        "total_size": prepared.manifest.total_size,  # type: ignore[attr-defined]
        "manifest_sha256": prepared.manifest.sha256,  # type: ignore[attr-defined]
        "path": str(prepared.root),  # type: ignore[attr-defined]
    }


def _nonce_from_args(args: argparse.Namespace) -> str:
    if args.nonce:
        return args.nonce
    try:
        return parse_challenge(args.challenge_file.read_bytes()).nonce
    except OSError as exc:
        raise DeploymentError(f"could not read challenge file: {exc}") from exc


def _prepare(args: argparse.Namespace, *, nonce: str, output: Path, action: str) -> object:
    return prepare_job(
        vpk_path=args.vpk,
        output_directory=output,
        nonce=nonce,
        private_key=args.private_key,
        head_template=args.head_template,
        action=action,
        installer_title_id=args.installer_title,
        job_id=getattr(args, "job_id", None),
        crypto_backend=args.crypto_backend,
        openssl_executable=args.openssl,
        limits=_limits(args),
    )


def _read_previous_challenge(ftp: VitaFtpClient) -> str | None:
    try:
        return ftp.read_challenge().nonce
    except VitaDevDeployError:
        return None


def _reconcile_terminal_install_failure(
    args: argparse.Namespace, *, job_root: Path
) -> object:
    """Read durable promotion evidence after a terminal install failure.

    A failed result can coexist with ``promote.state`` when restoration or
    marker cleanup failed, or after the installer was dispatched. Failure to
    read that state is itself retry-unsafe and must preserve the signed job.
    """

    try:
        with VitaFtpClient(
            args.vita, port=args.ftp_port, timeout=args.network_timeout
        ) as ftp:
            return collect_recovery_snapshot(ftp)
    except (VitaDevDeployError, OSError) as exc:
        raise DeploymentError(
            "terminal installation failure was received, but recovery-status "
            f"could not be read; signed job evidence is retained at {job_root}: {exc}"
        ) from exc


def _acquire_agent_challenge(
    ftp: VitaFtpClient,
    companion: VitaCompanionClient,
    *,
    reuse_running_agent: bool,
    installer_title: str,
    network_timeout: float,
) -> tuple[Challenge, str | None, str]:
    """Return a challenge without disrupting an explicitly reused agent."""

    if reuse_running_agent:
        try:
            challenge = ftp.read_challenge()
        except VitaDevDeployError as exc:
            raise DeploymentError(
                "--reuse-running-agent requires a valid current challenge; "
                "confirm the agent is open and showing that it is waiting"
            ) from exc
        # Do not even query the command port here. On affected systems a
        # Companion command can disturb an otherwise healthy waiting agent.
        return challenge, None, "reused"

    companion_version = companion.version()
    previous_nonce = _read_previous_challenge(ftp)
    # Never force-close the current application. In particular, abrupt
    # teardown can bypass framebuffer and other resource cleanup in homebrew.
    # The caller is responsible for leaving the Vita at LiveArea first.
    companion.launch(installer_title)
    challenge = ftp.wait_for_challenge(
        previous_nonce=previous_nonce,
        timeout=network_timeout + 5.0,
    )
    return challenge, companion_version, "launched"


def _deploy(args: argparse.Namespace) -> dict[str, object]:
    inspection = inspect_vpk(args.vpk, _limits(args))
    action = "verify" if args.verify_only else args.action
    if args.dry_run:
        if args.output is not None:
            job = _prepare(args, nonce=secrets.token_hex(32), output=args.output, action=action)
            summary = _job_summary(job)
        else:
            with tempfile.TemporaryDirectory(prefix="vitadevdeploy-dry-run-") as directory:
                job = _prepare(args, nonce=secrets.token_hex(32), output=Path(directory), action=action)
                summary = _job_summary(job)
                summary["path"] = "(temporary dry-run job removed)"
        summary["mode"] = "dry-run"
        summary["network_access"] = False
        return summary

    companion = VitaCompanionClient(args.vita, port=args.command_port, timeout=args.network_timeout)
    temporary_output = args.output is None
    retain_temporary_output = False
    prepared_job = None
    if args.output is None:
        output = Path(tempfile.mkdtemp(prefix="vitadevdeploy-job-"))
    else:
        output = args.output
    try:
        direct_receipt = None
        direct_commit_state = None
        direct_transfer_was_ambiguous = False
        recovered_after_ambiguous_commit = False
        recovery_snapshot = None
        if args.transport == "tcp":
            direct = VitaDirectClient(
                args.vita,
                port=args.tcp_port,
                timeout=args.network_timeout,
            )
            try:
                if args.reuse_running_agent:
                    companion_version = None
                    agent_session = "reused"
                else:
                    companion_version = companion.version()
                    # As with FTP deployment, the caller must first leave the
                    # Vita at LiveArea. Never force-close another application.
                    companion.launch(args.installer_title)
                    agent_session = "launched"
                direct.connect_wait(timeout=args.network_timeout + 5.0)
                if direct.challenge is None:
                    raise DeploymentError("direct TCP intake did not publish a challenge")
                challenge = direct.challenge
                # The receiver starts one absolute 30-second pre-authentication
                # deadline after sending its greeting. Close this challenge-
                # discovery connection before VPK extraction, hashing, manifest
                # generation, and signing. The one-shot agent keeps the same
                # live challenge while it accepts the subsequent connection.
                direct.close()
                job = _prepare(args, nonce=challenge.nonce, output=output, action=action)
                prepared_job = job
                # Once a signed direct job exists, retain it by default.  It is
                # deleted only after a validated terminal result (plus clean
                # recovery status for a failed install). This also survives
                # KeyboardInterrupt/SystemExit without catching or replacing
                # the original BaseException.
                retain_temporary_output = temporary_output
                direct_plan = prepare_direct_transfer(job)
                try:
                    direct.connect_wait(timeout=args.network_timeout + 5.0)
                    direct_receipt = direct.stage_job(direct_plan)
                    direct_commit_state = direct_receipt.commit_state
                except DirectTransferError as exc:
                    direct_commit_state = exc.commit_state
                    direct_transfer_was_ambiguous = (
                        exc.commit_state == DirectCommitState.AMBIGUOUS_AFTER_COMMIT
                    )
                    if exc.commit_state == DirectCommitState.FAIL_BEFORE_COMMIT:
                        # Direct-v1 negative ACKs are not authenticated or bound
                        # to this job/nonce. Preserve every signed job after a
                        # transfer attempt, even when the receiver claims it
                        # remained before commit.
                        if temporary_output:
                            raise DirectTransferError(
                                f"{exc}; retained signed job evidence at {job.root}",
                                commit_state=exc.commit_state,
                                job=job.request.job,
                                result_may_appear=exc.result_may_appear,
                            ) from exc
                        raise
                    if not exc.result_may_appear:
                        raise DirectTransferError(
                            f"{exc}; retained signed job evidence at {job.root}",
                            commit_state=exc.commit_state,
                            job=job.request.job,
                        ) from exc
            finally:
                direct.close()
            remote_job = f"{DEFAULT_REMOTE_ROOT}/inbox/{job.request.job}"
            # Protocol v1 keeps the small durable result on Vita storage. The
            # direct increment removes FTP from package transfer; result
            # polling remains compatible with installed Companion 1.06.
            try:
                with VitaFtpClient(
                    args.vita,
                    port=args.ftp_port,
                    timeout=args.network_timeout,
                ) as ftp:
                    result = ftp.poll_result(job.request.job, timeout=args.result_timeout)
                recovered_after_ambiguous_commit = direct_transfer_was_ambiguous
            except VitaDevDeployError as exc:
                # A known commit whose result channel failed, and every
                # ambiguous post-payload failure, must retain the signed job.
                # Its job ID is the only stable handle for later reconciliation.
                retain_temporary_output = temporary_output
                state = direct_commit_state or DirectCommitState.AMBIGUOUS_AFTER_COMMIT
                raise DirectTransferError(
                    f"direct deployment is {state.value}; retained signed job evidence "
                    f"at {job.root}; durable result polling failed: {exc}",
                    commit_state=state,
                    job=job.request.job,
                    result_may_appear=True,
                ) from exc
        else:
            with VitaFtpClient(args.vita, port=args.ftp_port, timeout=args.network_timeout) as ftp:
                challenge, companion_version, agent_session = _acquire_agent_challenge(
                    ftp,
                    companion,
                    reuse_running_agent=args.reuse_running_agent,
                    installer_title=args.installer_title,
                    network_timeout=args.network_timeout,
                )
                job = _prepare(args, nonce=challenge.nonce, output=output, action=action)
                remote_job = ftp.stage_job(job)
                result = ftp.poll_result(job.request.job, timeout=args.result_timeout)
        if result.title_id != inspection.title_id:
            detail = (
                f"; retained signed job evidence at {job.root}"
                if args.transport == "tcp" and temporary_output
                else ""
            )
            if args.transport == "tcp":
                raise DirectTransferError(
                    "installer result TITLE_ID does not match the prepared package" + detail,
                    commit_state=(
                        direct_commit_state
                        or DirectCommitState.AMBIGUOUS_AFTER_COMMIT
                    ),
                    job=job.request.job,
                )
            raise DeploymentError(
                "installer result TITLE_ID does not match the prepared package" + detail
            )
        if args.transport == "tcp":
            direct_commit_state = DirectCommitState.KNOWN_COMMITTED
            if not result.succeeded and action != "verify":
                try:
                    recovery_snapshot = _reconcile_terminal_install_failure(
                        args, job_root=job.root
                    )
                except VitaDevDeployError:
                    retain_temporary_output = temporary_output
                    raise
            # A matching terminal result closes the transfer ambiguity. An
            # install failure is deletion-safe only when recovery-status also
            # proves that no promotion marker remains.
            retain_temporary_output = bool(
                temporary_output
                and recovery_snapshot is not None
                and not recovery_snapshot.safe_to_retry
            )
        if not result.succeeded:
            recovery_detail = ""
            if recovery_snapshot is not None and not recovery_snapshot.safe_to_retry:
                recovery_detail = (
                    f"; recovery-status={recovery_snapshot.disposition}; signed job "
                    f"evidence is retained at {job.root}; "
                    f"{recovery_snapshot.operator_action}"
                )
            raise DeploymentError(
                f"Vita deployment failed during {result.stage} with code {result.code}: "
                f"{result.message}{recovery_detail}"
            )
        launch_response = None
        if action == "install_launch":
            # A successful agent intentionally remains visible for two seconds
            # after committing its result. Let it finish cleanup and exit on
            # its own before asking SceShell to launch the installed title.
            time.sleep(AGENT_SUCCESS_EXIT_GRACE_SECONDS)
            if companion_version is None:
                companion_version = companion.version()
            launch_response = companion.launch(inspection.title_id)
        summary = _job_summary(job)
        summary.update(
            {
                "mode": "deploy",
                "transport": args.transport,
                "remote_job": remote_job,
                "agent_session": agent_session,
                "companion_version": companion_version,
                "result_state": result.state,
                "result_stage": result.stage,
                "result_code": result.code,
                "result_message": result.message,
                "launch": launch_response,
            }
        )
        if direct_receipt is not None:
            summary.update(
                {
                    "direct_tcp_port": args.tcp_port,
                    "direct_resumed_files": direct_receipt.resumed_files,
                    "direct_resumed_bytes": direct_receipt.resumed_bytes,
                    "direct_committed_files": direct_receipt.committed_files,
                    "direct_committed_bytes": direct_receipt.committed_bytes,
                }
            )
        if direct_commit_state is not None:
            summary["direct_commit_state"] = direct_commit_state.value
            summary["direct_recovered_after_ambiguous_commit"] = (
                recovered_after_ambiguous_commit
            )
        return summary
    except BaseException as exc:
        if (
            temporary_output
            and retain_temporary_output
            and prepared_job is not None
            and str(prepared_job.root) not in str(exc)
        ):
            print(
                "VitaDevDeploy interrupted; retained signed job evidence "
                f"for {prepared_job.request.job} at {prepared_job.root}",
                file=sys.stderr,
            )
        raise
    finally:
        if temporary_output and not retain_temporary_output:
            shutil.rmtree(output, ignore_errors=True)


def _resume_direct(args: argparse.Namespace) -> dict[str, object]:
    job = load_prepared_job(args.job)
    result = None
    receipt = None
    with VitaFtpClient(
        args.vita, port=args.ftp_port, timeout=args.network_timeout
    ) as ftp:
        result = ftp.read_result(job.request.job)
    result_was_existing = result is not None
    recovered_after_ambiguous_commit = False
    direct_commit_state: DirectCommitState | None = None
    if result is None:
        # Loading, manifest verification, and full package hashing happen only
        # when retransmission is needed and before a live receiver connection
        # starts its 30-second authentication deadline.
        direct_plan = prepare_direct_transfer(job)
        direct_error: DirectTransferError | None = None
        direct = VitaDirectClient(
            args.vita, port=args.tcp_port, timeout=args.network_timeout
        )
        try:
            try:
                direct.connect_wait(timeout=args.network_timeout)
                receipt = direct.stage_job(direct_plan)
                direct_commit_state = receipt.commit_state
            except DirectTransferError as exc:
                direct_error = exc
                direct_commit_state = exc.commit_state
            except VitaDevDeployError as exc:
                # Failure to reconnect can mean the previous connection
                # committed and the one-shot agent is processing the job.
                direct_error = DirectTransferError(
                    str(exc),
                    commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                    job=job.request.job,
                    result_may_appear=True,
                )
                direct_commit_state = DirectCommitState.AMBIGUOUS_AFTER_COMMIT
        finally:
            direct.close()
        if direct_error is not None:
            if direct_error.commit_state == DirectCommitState.FAIL_BEFORE_COMMIT:
                # A phase-1 negative normally means no payload was authorized,
                # but ACK01 is unauthenticated. Preserve the retained job and
                # perform one result read to cover a replay/result that became
                # visible immediately before the response without imposing a
                # long wait for ordinary bad-signature or stale-nonce reports.
                with VitaFtpClient(
                    args.vita, port=args.ftp_port, timeout=args.network_timeout
                ) as ftp:
                    result = ftp.read_result(job.request.job)
                if result is None:
                    raise direct_error
            elif direct_error.result_may_appear:
                # Installation and full package verification can outlive the
                # reconnect timeout. Poll the durable result for the caller's
                # full result window instead of performing a racy single read.
                try:
                    with VitaFtpClient(
                        args.vita, port=args.ftp_port, timeout=args.network_timeout
                    ) as ftp:
                        result = ftp.poll_result(
                            job.request.job, timeout=args.result_timeout
                        )
                    recovered_after_ambiguous_commit = True
                except VitaDevDeployError as exc:
                    raise DirectTransferError(
                        f"direct deployment remains {direct_error.commit_state.value}; "
                        f"signed job evidence is retained at {job.root}; "
                        f"durable result polling failed: {exc}",
                        commit_state=direct_error.commit_state,
                        job=job.request.job,
                        result_may_appear=True,
                    ) from exc
            else:
                raise DirectTransferError(
                    f"{direct_error}; signed job evidence is retained at {job.root}",
                    commit_state=direct_error.commit_state,
                    job=job.request.job,
                ) from direct_error
        else:
            try:
                with VitaFtpClient(
                    args.vita, port=args.ftp_port, timeout=args.network_timeout
                ) as ftp:
                    result = ftp.poll_result(job.request.job, timeout=args.result_timeout)
            except VitaDevDeployError as exc:
                raise DirectTransferError(
                    f"direct deployment is {direct_commit_state.value}; "
                    f"signed job evidence is retained at {job.root}; "
                    f"durable result polling failed: {exc}",
                    commit_state=direct_commit_state,
                    job=job.request.job,
                    result_may_appear=True,
                ) from exc

    if result.title_id != job.request.title_id:
        raise DirectTransferError(
            "installer result TITLE_ID does not match the retained job; signed job "
            f"evidence remains at {job.root}",
            commit_state=(
                direct_commit_state or DirectCommitState.AMBIGUOUS_AFTER_COMMIT
            ),
            job=job.request.job,
        )
    direct_commit_state = DirectCommitState.KNOWN_COMMITTED
    if not result.succeeded:
        recovery_detail = ""
        if job.request.action != "verify":
            snapshot = _reconcile_terminal_install_failure(args, job_root=job.root)
            if not snapshot.safe_to_retry:
                recovery_detail = (
                    f"; recovery-status={snapshot.disposition}; signed job evidence "
                    f"remains at {job.root}; {snapshot.operator_action}"
                )
        raise DeploymentError(
            f"Vita deployment failed during {result.stage} with code {result.code}: "
            f"{result.message}{recovery_detail}"
        )
    launch_response = None
    companion_version = None
    if job.request.action == "install_launch":
        time.sleep(AGENT_SUCCESS_EXIT_GRACE_SECONDS)
        companion = VitaCompanionClient(
            args.vita, port=args.command_port, timeout=args.network_timeout
        )
        companion_version = companion.version()
        launch_response = companion.launch(job.request.title_id)
    summary = _job_summary(job)
    summary.update(
        {
            "mode": "resume-direct",
            "transport": "tcp",
            "direct_tcp_port": args.tcp_port,
            "result_state": result.state,
            "result_stage": result.stage,
            "result_code": result.code,
            "result_message": result.message,
            "existing_result": result_was_existing,
            "direct_commit_state": (
                direct_commit_state.value
                if direct_commit_state is not None
                else DirectCommitState.KNOWN_COMMITTED.value
            ),
            "direct_recovered_after_ambiguous_commit": (
                recovered_after_ambiguous_commit
            ),
            "companion_version": companion_version,
            "launch": launch_response,
        }
    )
    if receipt is not None:
        summary.update(
            {
                "direct_resumed_files": receipt.resumed_files,
                "direct_resumed_bytes": receipt.resumed_bytes,
                "direct_committed_files": receipt.committed_files,
                "direct_committed_bytes": receipt.committed_bytes,
            }
        )
    return summary


def _recovery_status(args: argparse.Namespace) -> dict[str, object]:
    with VitaFtpClient(
        args.vita, port=args.ftp_port, timeout=args.network_timeout
    ) as ftp:
        snapshot = collect_recovery_snapshot(ftp)
    result = snapshot.to_json()
    result["mode"] = "recovery-status"
    result["read_only"] = True
    return result


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "verify":
            inspection = inspect_vpk(args.vpk, _limits(args))
            result: dict[str, object] = {
                "mode": "verify-only",
                "title_id": inspection.title_id,
                "content_id": inspection.content_id,
                "file_count": inspection.file_count,
                "total_size": inspection.total_size,
            }
        elif args.command == "keygen":
            backend = generate_keypair(
                args.private,
                args.public,
                backend=args.crypto_backend,
                openssl_executable=args.openssl,
                force=args.force,
            )
            if args.raw_public is not None or args.c_header is not None:
                export_public_key(
                    args.public,
                    raw_path=args.raw_public,
                    header_path=args.c_header,
                    backend=args.crypto_backend,
                    openssl_executable=args.openssl,
                    force=args.force,
                )
            result = {"mode": "keygen", "backend": backend, "private": str(args.private), "public": str(args.public)}
        elif args.command == "export-public":
            raw = export_public_key(
                args.public,
                raw_path=args.raw,
                header_path=args.c_header,
                backend=args.crypto_backend,
                openssl_executable=args.openssl,
                force=args.force,
            )
            result = {"mode": "export-public", "fingerprint_sha256": __import__("hashlib").sha256(raw).hexdigest()}
        elif args.command == "prepare":
            result = _job_summary(_prepare(args, nonce=_nonce_from_args(args), output=args.output, action=args.action))
            result["mode"] = "prepare"
        elif args.command == "check-job":
            job = verify_prepared_job(
                args.job,
                args.public_key,
                crypto_backend=args.crypto_backend,
                openssl_executable=args.openssl,
            )
            result = _job_summary(job)
            result["mode"] = "check-job"
            result["signature_valid"] = True
        elif args.command == "deploy":
            result = _deploy(args)
        elif args.command == "resume-direct":
            result = _resume_direct(args)
        elif args.command == "recovery-status":
            result = _recovery_status(args)
        else:
            parser.error("unknown command")
            return 2
    except (VitaDevDeployError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
