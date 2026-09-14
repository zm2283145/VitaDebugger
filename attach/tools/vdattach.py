#!/usr/bin/env python3
"""Run the isolated attach host package without installing it."""

from pathlib import Path
import sys


HOST_ROOT = Path(__file__).resolve().parent.parent / "host"
sys.path.insert(0, str(HOST_ROOT))

from vdattach.cli import main  # noqa: E402


raise SystemExit(main())
