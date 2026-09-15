#!/usr/bin/env python3
"""Run the deterministic VitaDevDeploy display-lifecycle model."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


DEPLOY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEPLOY_ROOT / "host"))

from vitadevdeploy.gpu_lifecycle import run_lifecycle_model  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Exercise the deterministic display ownership model; this does not "
            "launch, close, or otherwise modify a Vita"
        )
    )
    parser.add_argument("--cycles", type=int, default=10000)
    parser.add_argument("--frames-per-cycle", type=int, default=8)
    args = parser.parse_args(argv)
    try:
        result = run_lifecycle_model(
            cycles=args.cycles, frames_per_cycle=args.frames_per_cycle
        )
    except ValueError as exc:
        parser.error(str(exc))
    result.update(
        {
            "mode": "host-model",
            "vita_access": False,
            "hardware_gate_complete": False,
        }
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
