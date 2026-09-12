#!/usr/bin/env python3
"""Print human-readable VitaDevDeploy startup diagnostics."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "host"))

from vitadevdeploy.startup import (  # noqa: E402
    StartupTraceError,
    format_startup_diagnostics,
    parse_startup_io_trace,
    parse_startup_record,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="decode VDD1 startup and optional VIO1 atomic-write traces"
    )
    parser.add_argument("startup", type=Path, help="pulled VitaDevDeploy.startup")
    parser.add_argument(
        "--io-trace", type=Path,
        help="pulled VitaDevDeploy.startup_io from the same agent launch",
    )
    args = parser.parse_args()

    try:
        startup = parse_startup_record(args.startup.read_bytes())
        io_trace = (
            parse_startup_io_trace(args.io_trace.read_bytes())
            if args.io_trace is not None else None
        )
    except (OSError, StartupTraceError) as exc:
        parser.error(str(exc))
    print(format_startup_diagnostics(startup, io_trace))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
