#!/usr/bin/env python3
"""Run the protocol-v2 authentication-only client without installation."""

from pathlib import Path
import sys


ATTACH_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = ATTACH_ROOT.parent
sys.path.insert(0, str(ATTACH_ROOT / "host"))
sys.path.insert(0, str(REPO_ROOT / "deploy" / "host"))

from vdattach.auth_cli import main  # noqa: E402


raise SystemExit(main())
