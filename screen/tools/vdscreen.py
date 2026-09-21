"""Repository entry point for the VitaDebugger screen receiver."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))

from vdscreen.__main__ import main


if __name__ == "__main__":
    raise SystemExit(main())
