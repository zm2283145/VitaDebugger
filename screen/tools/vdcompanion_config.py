"""Create one offline, explicit opt-in Vita companion endpoint config."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))

from vdscreen.companion import (  # noqa: E402
    CAP_APP_INPUT,
    CAP_DEBUG_FILES,
    CAP_INPUT_PLAYBACK,
    CAP_INPUT_RECORD,
    CAP_SCREEN,
    CAP_STATUS,
    decode_endpoint_config,
    encode_endpoint_config,
)


def _write_exclusive(path: Path, data: bytes) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0),
        0o600,
    )
    try:
        with os.fdopen(descriptor, "wb") as output:
            descriptor = -1
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="vdcompanion-config")
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--allow-lan", action="store_true")
    parser.add_argument(
        "--bind-address",
        help="exact Vita IPv4 to bind; required with --allow-lan",
    )
    parser.add_argument("--screen", action="store_true")
    parser.add_argument("--input", action="store_true")
    parser.add_argument("--debug-files", action="store_true")
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--playback", action="store_true")
    return parser


def main() -> int:
    args = _parser().parse_args()
    capabilities = CAP_STATUS
    capabilities |= CAP_SCREEN if args.screen else 0
    capabilities |= CAP_APP_INPUT if args.input else 0
    capabilities |= CAP_DEBUG_FILES if args.debug_files else 0
    capabilities |= CAP_INPUT_RECORD if args.record else 0
    capabilities |= CAP_INPUT_PLAYBACK if args.playback else 0
    control_secret = secrets.token_bytes(32)
    screen_secret = secrets.token_bytes(32)
    while secrets.compare_digest(control_secret, screen_secret):
        screen_secret = secrets.token_bytes(32)
    config = encode_endpoint_config(
        host=args.host,
        capabilities=capabilities,
        control_secret=control_secret,
        screen_secret=screen_secret,
        allow_lan=args.allow_lan,
        bind_address=args.bind_address,
    )
    decode_endpoint_config(config)

    output_directory = args.output_directory
    output_directory.mkdir(parents=True, exist_ok=True)
    outputs = (
        (output_directory / "config.bin", config),
        (output_directory / "control-secret.bin", control_secret),
        (output_directory / "screen-token.bin", screen_secret),
    )
    created: list[Path] = []
    try:
        for path, data in outputs:
            _write_exclusive(path, data)
            created.append(path)
    except BaseException:
        for path in reversed(created):
            path.unlink(missing_ok=True)
        raise
    print(json.dumps({
        "capabilities": capabilities,
        "config": str(outputs[0][0]),
        "control_port": 18198,
        "control_secret_sha256": hashlib.sha256(control_secret).hexdigest(),
        "hardware_contacted": False,
        "host": args.host,
        "bind_address": str(decode_endpoint_config(config).bind_address),
        "screen_port": 18197,
        "screen_token_sha256": hashlib.sha256(screen_secret).hexdigest(),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
