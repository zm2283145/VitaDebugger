#!/usr/bin/env python3
"""Accept one PMU gate prelude, force TCP reset, and journal the result."""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
from datetime import datetime, timezone
from pathlib import Path

PRELUDE = b"VDPMU-DROP-V1\n"


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_json_atomic(path: Path, evidence: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def run_receiver(
    bind: str, port: int, evidence_path: Path, timeout: float
) -> dict[str, object]:
    evidence: dict[str, object] = {
        "schema": "vitadebug-pmu-disconnect-receiver-v1",
        "bind": bind,
        "port": port,
        "started_utc": _utc_now(),
        "state": "listening",
    }
    _write_json_atomic(evidence_path, evidence)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind, port))
        evidence["port"] = listener.getsockname()[1]
        _write_json_atomic(evidence_path, evidence)
        listener.listen(1)
        listener.settimeout(timeout)
        connection, peer = listener.accept()
        with connection:
            connection.settimeout(timeout)
            received = bytearray()
            while len(received) < len(PRELUDE):
                chunk = connection.recv(len(PRELUDE) - len(received))
                if not chunk:
                    break
                received.extend(chunk)
            evidence.update(
                {
                    "accepted_utc": _utc_now(),
                    "peer": [peer[0], peer[1]],
                    "prelude_hex": bytes(received).hex(),
                }
            )
            if bytes(received) != PRELUDE:
                evidence["state"] = "invalid_prelude"
                evidence["finished_utc"] = _utc_now()
                _write_json_atomic(evidence_path, evidence)
                raise RuntimeError(
                    f"unexpected prelude {bytes(received)!r}"
                )
            linger_format = "hh" if os.name == "nt" else "ii"
            connection.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                struct.pack(linger_format, 1, 0),
            )
        evidence["state"] = "reset_sent"
        evidence["finished_utc"] = _utc_now()
        _write_json_atomic(evidence_path, evidence)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18196)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in [1, 65535]")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    run_receiver(args.bind, args.port, args.evidence, args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
