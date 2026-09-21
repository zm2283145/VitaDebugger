"""Command line entry point for the VitaDebugger screen receiver."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .protocol import AUTH_TOKEN_SIZE, ProtocolError
from .receiver import (
    DEFAULT_LISTENER_PORT,
    LatestFrameStore,
    ReceiverLimits,
    listen_once,
    read_latest,
    validate_listener_port,
)


def _load_token(path: Path) -> bytes:
    raw = path.read_bytes()
    stripped = raw.strip()
    if len(stripped) == 64:
        try:
            raw = bytes.fromhex(stripped.decode("ascii"))
        except (UnicodeDecodeError, ValueError) as error:
            raise ValueError("token file is not 32 raw bytes or 64 hex digits") from error
    if len(raw) != AUTH_TOKEN_SIZE or not any(raw):
        raise ValueError("token file must contain exactly 32 nonzero raw bytes or 64 hex digits")
    return raw


def _receive(args: argparse.Namespace) -> int:
    limits = ReceiverLimits(
        max_width=args.max_width,
        max_height=args.max_height,
        max_payload_bytes=args.max_payload_bytes,
        max_frames_per_second=args.max_fps,
        rate_burst_frames=args.rate_burst,
        idle_timeout_seconds=args.idle_timeout,
        max_session_seconds=args.max_session_seconds,
    )
    limits.validate()
    token = _load_token(args.token_file)
    store = LatestFrameStore(args.output)
    stats = listen_once(
        bind=args.bind,
        port=args.port,
        allow_lan=args.allow_lan,
        accept_timeout_seconds=args.accept_timeout,
        token=token,
        store=store,
        limits=limits,
    )
    print(json.dumps(vars(stats), sort_keys=True))
    return 0


def _latest(args: argparse.Namespace) -> int:
    metadata, _ = read_latest(args.output)
    print(json.dumps(metadata, sort_keys=True))
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="vdscreen")
    commands = parser.add_subparsers(dest="command", required=True)
    receive = commands.add_parser("receive", help="receive one bounded session")
    receive.add_argument("--token-file", type=Path, required=True)
    receive.add_argument("--output", type=Path, required=True)
    receive.add_argument("--bind", default="127.0.0.1")
    receive.add_argument("--port", type=int, default=DEFAULT_LISTENER_PORT)
    receive.add_argument("--allow-lan", action="store_true")
    receive.add_argument("--max-width", type=int, default=960)
    receive.add_argument("--max-height", type=int, default=544)
    receive.add_argument("--max-payload-bytes", type=int, default=4 * 1024 * 1024)
    receive.add_argument("--max-fps", type=float, default=10.0)
    receive.add_argument("--rate-burst", type=int, default=2)
    receive.add_argument("--accept-timeout", type=float, default=30.0)
    receive.add_argument("--idle-timeout", type=float, default=3.0)
    receive.add_argument("--max-session-seconds", type=float, default=3600.0)
    receive.set_defaults(run=_receive)
    latest = commands.add_parser("latest", help="verify and print latest metadata")
    latest.add_argument("--output", type=Path, required=True)
    latest.set_defaults(run=_latest)
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        validate_listener_port(getattr(args, "port", DEFAULT_LISTENER_PORT))
    except ValueError as error:
        raise SystemExit(f"vdscreen: {error}") from error
    if not 0 < getattr(args, "accept_timeout", 30.0) <= 3600:
        raise SystemExit("accept timeout must be between 0 and 3600 seconds")
    try:
        return args.run(args)
    except (OSError, ProtocolError, ValueError) as error:
        raise SystemExit(f"vdscreen: {error}") from error


if __name__ == "__main__":
    raise SystemExit(main())
