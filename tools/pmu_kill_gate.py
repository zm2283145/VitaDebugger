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
ARMED_PATH = "ux0:/data/VitaDebugger/pmu-cleanup-v1-abrupt-exit-b.bin"


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


def _read_optional(ftp: ftplib.FTP, path: str) -> bytes | None:
    chunks: list[bytes] = []
    try:
        ftp.retrbinary(f"RETR {path}", chunks.append)
    except ftplib.error_perm as exc:
        if str(exc).startswith("550"):
            return None
        raise
    return b"".join(chunks)


def run_kill_gate(
    vita: str,
    evidence_path: Path,
    armed_copy: Path,
    timeout: float,
    poll_interval: float,
    *,
    ftp_factory: Callable[[], ftplib.FTP] = ftplib.FTP,
    companion_factory: Callable[..., VitaCompanionClient] = VitaCompanionClient,
) -> dict[str, object]:
    if evidence_path.exists() or armed_copy.exists():
        raise FileExistsError(
            "evidence and armed-copy paths must both be new"
        )
    evidence: dict[str, object] = {
        "schema": "vitadebug-pmu-kill-gate-v1",
        "vita": vita,
        "title_id": TITLE_ID,
        "armed_path": ARMED_PATH,
        "started_utc": _utc_now(),
        "state": "checking_absence",
    }
    _write_json_atomic(evidence_path, evidence)
    ftp = ftp_factory()
    try:
        ftp.connect(vita, 1337, timeout=min(timeout, 5.0))
        ftp.login()
        ftp.voidcmd("TYPE I")
        if _read_optional(ftp, ARMED_PATH) is not None:
            raise RuntimeError(
                "armed journal already exists; refusing to kill"
            )
        evidence["absence_confirmed_utc"] = _utc_now()
        evidence["state"] = "waiting_for_armed"
        _write_json_atomic(evidence_path, evidence)
        deadline = time.monotonic() + timeout
        data = None
        while time.monotonic() < deadline:
            time.sleep(poll_interval)
            candidate = _read_optional(ftp, ARMED_PATH)
            if candidate is None or len(candidate) != SIZE:
                continue
            data = candidate
            break
        if data is None:
            raise TimeoutError("new armed journal did not appear")
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
        _write_bytes_atomic(armed_copy, data)
        evidence.update(
            {
                "armed_verified_utc": _utc_now(),
                "armed_sha256": decoded["file_sha256"],
                "armed_copy": str(armed_copy),
                "state": "armed_verified",
            }
        )
        _write_json_atomic(evidence_path, evidence)
        response = companion_factory(
            vita, timeout=min(timeout, 5.0)
        ).kill(TITLE_ID, require_success=True)
        evidence.update(
            {
                "kill_reply": response,
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
            ftp.quit()
        except (OSError, ftplib.Error):
            try:
                ftp.close()
            except OSError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vita", required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--armed-copy", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--poll-interval", type=float, default=0.05)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not 0 < args.poll_interval <= 1:
        parser.error("--poll-interval must be in (0, 1]")
    run_kill_gate(
        args.vita, args.evidence, args.armed_copy,
        args.timeout, args.poll_interval
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
