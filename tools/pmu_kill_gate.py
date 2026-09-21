#!/usr/bin/env python3
"""Observe a new stage-5 armed journal, then kill only VDCP00013."""

from __future__ import annotations

import argparse
import ftplib
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from deploy.host.vitadevdeploy.companion import VitaCompanionClient
from tools.decode_pmu_cleanup_record import SIZE, decode_record

TITLE_ID = "VDCP00013"
ARMED_PATH = "ux0:/data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-b.bin"
FAILED_PATH = "ux0:/data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-c.bin"
MAX_KILL_DELAY_SECONDS = 2.0


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_bytes_atomic(path: Path, data: bytes) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _write_json_atomic(path: Path, evidence: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _remaining(
    deadline: float,
    monotonic: Callable[[], float],
    operation: str,
) -> float:
    remaining = deadline - monotonic()
    if remaining <= 0:
        raise TimeoutError(f"deadline expired before {operation}")
    return remaining


def _prepare_ftp(
    ftp: ftplib.FTP,
    deadline: float,
    monotonic: Callable[[], float],
    operation: str,
) -> float:
    remaining = _remaining(deadline, monotonic, operation)
    ftp.timeout = remaining
    sock = getattr(ftp, "sock", None)
    if sock is not None:
        sock.settimeout(remaining)
    return remaining


def _read_optional_before(
    ftp: ftplib.FTP,
    path: str,
    deadline: float,
    monotonic: Callable[[], float],
    operation: str,
) -> bytes | None:
    chunks: list[bytes] = []
    connection = None
    try:
        _prepare_ftp(ftp, deadline, monotonic, operation)
        connection = ftp.transfercmd(f"RETR {path}")
        while True:
            remaining = _remaining(
                deadline, monotonic, operation
            )
            connection.settimeout(remaining)
            block = connection.recv(8192)
            _remaining(deadline, monotonic, operation)
            if not block:
                break
            chunks.append(block)
        connection.close()
        connection = None
        _prepare_ftp(
            ftp, deadline, monotonic,
            f"{operation} final response",
        )
        ftp.voidresp()
    except ftplib.error_perm as exc:
        if str(exc).startswith("550") and not chunks:
            return None
        raise
    finally:
        if connection is not None:
            connection.close()
    return b"".join(chunks)


def run_kill_gate(
    vita: str,
    evidence_path: Path,
    armed_copy: Path,
    timeout: float,
    poll_interval: float,
    max_kill_delay: float = 2.0,
    *,
    ftp_factory: Callable[[], ftplib.FTP] = ftplib.FTP,
    companion_factory: Callable[..., VitaCompanionClient] = VitaCompanionClient,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, object]:
    if timeout <= 0:
        raise ValueError("timeout must be positive")
    if not 0 < poll_interval <= 1:
        raise ValueError("poll_interval must be in (0, 1]")
    if not 0 < max_kill_delay <= MAX_KILL_DELAY_SECONDS:
        raise ValueError(
            "max_kill_delay must be in (0, 2]"
        )
    if evidence_path.exists() or armed_copy.exists():
        raise FileExistsError(
            "evidence and armed-copy paths must both be new"
        )
    evidence: dict[str, object] = {
        "schema": "vitadebug-pmu-kill-gate-v2",
        "vita": vita,
        "title_id": TITLE_ID,
        "armed_path": ARMED_PATH,
        "failed_path": FAILED_PATH,
        "started_utc": _utc_now(),
        "state": "checking_absence",
    }
    operation_deadline = monotonic() + timeout
    _remaining(
        operation_deadline, monotonic, "initial evidence write"
    )
    _write_json_atomic(evidence_path, evidence)
    ftp = ftp_factory()
    try:
        ftp.connect(
            vita,
            1337,
            timeout=_prepare_ftp(
                ftp, operation_deadline, monotonic, "FTP connect"
            ),
        )
        _prepare_ftp(
            ftp, operation_deadline, monotonic, "FTP login"
        )
        ftp.login()
        _prepare_ftp(
            ftp, operation_deadline, monotonic, "FTP binary mode"
        )
        ftp.voidcmd("TYPE I")
        if _read_optional_before(
            ftp, ARMED_PATH, operation_deadline, monotonic,
            "initial armed-slot check",
        ) is not None:
            raise RuntimeError(
                "armed journal already exists; refusing to kill"
            )
        if _read_optional_before(
            ftp, FAILED_PATH, operation_deadline, monotonic,
            "initial failed-slot check",
        ) is not None:
            raise RuntimeError(
                "failed journal already exists; refusing to kill"
            )
        evidence["absence_confirmed_utc"] = _utc_now()
        evidence["state"] = "waiting_for_armed"
        _remaining(
            operation_deadline, monotonic,
            "waiting evidence write",
        )
        _write_json_atomic(evidence_path, evidence)
        data = None
        while True:
            remaining = _remaining(
                operation_deadline, monotonic, "armed-record poll"
            )
            sleep(min(poll_interval, remaining))
            if _read_optional_before(
                ftp, FAILED_PATH, operation_deadline, monotonic,
                "failed-slot poll",
            ) is not None:
                raise RuntimeError(
                    "device failed before a fresh armed record was observed"
                )
            candidate = _read_optional_before(
                ftp, ARMED_PATH, operation_deadline, monotonic,
                "armed-slot poll",
            )
            if candidate is None or len(candidate) != SIZE:
                continue
            data = candidate
            break
        _remaining(
            operation_deadline, monotonic, "armed-record decode"
        )
        decoded = decode_record(data)
        if (
            not decoded["valid"]
            or decoded["stage"] != 5
            or decoded["state"] != 2
            or decoded["revision"] != 2
        ):
            raise RuntimeError(
                "new journal is not a valid stage-5 armed record"
            )
        armed_observed = monotonic()
        kill_deadline = min(
            operation_deadline, armed_observed + max_kill_delay
        )
        _remaining(
            kill_deadline, monotonic, "armed evidence archive"
        )
        _write_bytes_atomic(armed_copy, data)
        evidence.update(
            {
                "armed_verified_utc": _utc_now(),
                "armed_sha256": decoded["file_sha256"],
                "armed_copy": str(armed_copy),
                "state": "armed_verified",
            }
        )
        _remaining(
            kill_deadline, monotonic, "armed evidence write"
        )
        _write_json_atomic(evidence_path, evidence)

        def verify_active_at_send() -> None:
            if _read_optional_before(
                ftp, FAILED_PATH, kill_deadline, monotonic,
                "final failed-slot check",
            ) is not None:
                raise RuntimeError(
                    "device active window expired before kill"
                )

        companion_timeout = _remaining(
            kill_deadline, monotonic, "Vita Companion connect"
        )
        response = companion_factory(
            vita, timeout=companion_timeout
        ).kill(
            TITLE_ID,
            require_success=True,
            deadline=kill_deadline,
            monotonic=monotonic,
            before_send=verify_active_at_send,
        )
        kill_delay = monotonic() - armed_observed
        evidence.update(
            {
                "kill_reply": response,
                "armed_to_kill_seconds": kill_delay,
            }
        )
        if kill_delay > max_kill_delay:
            raise RuntimeError(
                "confirmed kill exceeded the active-lease delay bound"
            )
        evidence.update(
            {
                "finished_utc": _utc_now(),
                "state": "kill_confirmed",
            }
        )
        _write_json_atomic(evidence_path, evidence)
        return evidence
    except Exception as exc:
        evidence.update(
            {
                "finished_utc": _utc_now(),
                "state": "failed",
                "error": f"{type(exc).__name__}: {exc}",
            }
        )
        _write_json_atomic(evidence_path, evidence)
        raise
    finally:
        try:
            _prepare_ftp(
                ftp, operation_deadline, monotonic, "FTP quit"
            )
            ftp.quit()
        except (OSError, ftplib.Error):
            try:
                ftp.close()
            except OSError:
                pass
        except TimeoutError:
            ftp.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vita", required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--armed-copy", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--poll-interval", type=float, default=0.05)
    parser.add_argument("--max-kill-delay", type=float, default=2.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not 0 < args.poll_interval <= 1:
        parser.error("--poll-interval must be in (0, 1]")
    if not 0 < args.max_kill_delay <= MAX_KILL_DELAY_SECONDS:
        parser.error("--max-kill-delay must be in (0, 2]")
    run_kill_gate(
        args.vita, args.evidence, args.armed_copy,
        args.timeout, args.poll_interval, args.max_kill_delay
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
