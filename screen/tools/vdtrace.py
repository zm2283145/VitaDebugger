"""Verify, inspect, and atomically save VitaDebugger input traces."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))

from vdscreen.trace import listing_json, save_trace, verify_trace


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("verify", "list"):
        child = subparsers.add_parser(command)
        child.add_argument("trace", type=Path)
    save = subparsers.add_parser("save")
    save.add_argument("trace", type=Path)
    save.add_argument("destination", type=Path)
    args = parser.parse_args(argv)
    data = args.trace.read_bytes()
    if args.command == "save":
        trace = save_trace(data, args.destination)
    else:
        trace = verify_trace(data)
    if args.command == "list":
        sys.stdout.write(listing_json(trace))
    else:
        print(
            f"valid trace {trace.trace_id}: {len(trace.events)} events, "
            f"{trace.duration_us} us, end reason {trace.end_reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
