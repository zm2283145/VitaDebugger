"""Authentication-only protocol-v2 command line client."""

from __future__ import annotations

import argparse
import ipaddress
import json
import socket
import sys
from pathlib import Path

from vitadevdeploy.crypto import get_backend

from .auth import AuthenticatedAttachClient
from .auth_keys import JsonKeyStore
from .client import checked_timeout
from .errors import AttachError


DEFAULT_PORT = 18195
MIN_PORT = 18000
MAX_PORT = 18999


def _private_ipv4(value: str) -> str:
    try:
        address = ipaddress.ip_address(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "host must be a canonical private IPv4 address"
        ) from exc
    private_networks = (
        ipaddress.IPv4Network("10.0.0.0/8"),
        ipaddress.IPv4Network("172.16.0.0/12"),
        ipaddress.IPv4Network("192.168.0.0/16"),
    )
    if not isinstance(address, ipaddress.IPv4Address) or not any(
        address in network for network in private_networks
    ) or str(address) != value:
        raise argparse.ArgumentTypeError(
            "host must be a canonical private IPv4 address"
        )
    return value


def _listener_port(value: str) -> int:
    try:
        port = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "port must be a decimal integer"
        ) from exc
    if not MIN_PORT <= port <= MAX_PORT:
        raise argparse.ArgumentTypeError(
            f"port must be from {MIN_PORT} through {MAX_PORT}"
        )
    return port


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "mutually authenticate to the VitaDebugger protocol-v2 listener; "
            "transport is signed plaintext, not encrypted"
        )
    )
    parser.add_argument("--host", required=True, type=_private_ipv4)
    parser.add_argument(
        "--port",
        type=_listener_port,
        default=DEFAULT_PORT,
    )
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--key-store", required=True, type=Path)
    parser.add_argument(
        "--crypto-backend",
        choices=("auto", "cryptography", "openssl"),
        default="auto",
    )
    parser.add_argument("--json", action="store_true")
    return parser


def run(args: argparse.Namespace) -> int:
    timeout = checked_timeout(args.timeout)
    store = JsonKeyStore(args.key_store, get_backend(args.crypto_backend))
    stream = socket.create_connection((args.host, args.port), timeout)
    with AuthenticatedAttachClient(
        stream, store, operation_timeout=timeout
    ) as client:
        session = client.authenticate()
    output = {
        "status": "authenticated",
        "transport": "signed-plaintext",
        "encrypted": False,
        "service_generation": session.service_generation,
        "session_id": session.session_id,
        "host_key_id": f"{session.host_key_id:016x}",
        "host_key_generation": session.host_key_generation,
        "server_key_id": f"{session.server_key_id:016x}",
        "server_key_generation": session.server_key_generation,
        "expires_at_ms": session.expires_at_ms,
    }
    if args.json:
        print(json.dumps(output, indent=2, sort_keys=True))
    else:
        for key, value in output.items():
            print(f"{key}: {value}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (AttachError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
