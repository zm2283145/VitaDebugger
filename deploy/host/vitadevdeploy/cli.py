"""Command-line interface for local preparation and Vita deployment."""

from __future__ import annotations

import argparse
import json
import secrets
import sys
import tempfile
import time
from pathlib import Path

from .companion import VitaCompanionClient
from .crypto import export_public_key, generate_keypair
from .errors import DeploymentError, VitaDevDeployError
from .ftp import VitaFtpClient
from .job import prepare_job, verify_prepared_job
from .protocol import DEPLOYER_TITLE_ID, Challenge, parse_challenge
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
    deploy_parser.add_argument("--ftp-port", type=int, default=1337)
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
    output_context = None
    if args.output is None:
        output_context = tempfile.TemporaryDirectory(prefix="vitadevdeploy-job-")
        output = Path(output_context.name)
    else:
        output = args.output
    try:
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
            raise DeploymentError("installer result TITLE_ID does not match the prepared package")
        if not result.succeeded:
            raise DeploymentError(
                f"Vita deployment failed during {result.stage} with code {result.code}: {result.message}"
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
        return summary
    finally:
        if output_context is not None:
            output_context.cleanup()


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
