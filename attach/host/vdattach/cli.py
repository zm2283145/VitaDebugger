"""Command-line entry point for the read-only attach discovery scaffold."""

from __future__ import annotations

import argparse
import json
import sys

from .client import AttachDiscoveryClient, canonical_ipv4, checked_port, checked_timeout
from .errors import AttachError
from .protocol import CURRENT_KERNEL_ABI, validate_title_id


def _number(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected an integer such as 0x0001000b") from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "read-only VitaDebugger attach discovery scaffold; this tool cannot "
            "inject a module, stop a process, write memory, or start GDB"
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("status", "perform only the bounded capability handshake"),
        ("discover", "resolve one exact title ID, report identity, then release its ticket"),
    ):
        command = subparsers.add_parser(name, help=help_text)
        command.add_argument("--host", required=True, help="canonical IPv4 address of a future attach broker")
        command.add_argument("--port", required=True, type=int, help="explicit attach broker TCP port")
        command.add_argument("--timeout", type=float, default=3.0)
        command.add_argument(
            "--expected-kernel-abi",
            type=_number,
            default=CURRENT_KERNEL_ABI,
            help=f"exact required kernel ABI (default 0x{CURRENT_KERNEL_ABI:08x})",
        )
        command.add_argument("--json", action="store_true")
        if name == "discover":
            command.add_argument("--title-id", required=True)
    return parser


def _validated_connection_args(args: argparse.Namespace) -> None:
    canonical_ipv4(args.host)
    checked_port(args.port)
    checked_timeout(args.timeout)
    if not 1 <= args.expected_kernel_abi <= 0xFFFFFFFF:
        raise AttachError("expected kernel ABI must be a nonzero 32-bit value")
    if args.command == "discover":
        validate_title_id(args.title_id)


def run(args: argparse.Namespace) -> int:
    _validated_connection_args(args)
    with AttachDiscoveryClient.connect(
        args.host,
        args.port,
        timeout=args.timeout,
        expected_kernel_abi=args.expected_kernel_abi,
    ) as client:
        hello = client.hello()
        output: dict[str, object] = {
            "mode": "observe",
            "state": hello.state,
            "kernel_abi": f"0x{hello.kernel_abi:08x}",
            "kernel_caps": f"0x{hello.kernel_caps:08x}",
            "attach_caps": f"0x{hello.attach_caps:08x}",
            "control_policy": "disabled",
        }
        if args.command == "discover":
            snapshot = client.discover(args.title_id)
            release = client.release()
            output.update(
                {
                    "title_id": snapshot.title_id,
                    "pid": f"0x{snapshot.pid:08x}",
                    "main_modid": f"0x{snapshot.main_modid:08x}",
                    "main_fingerprint": f"0x{snapshot.main_fingerprint:08x}",
                    "target_generation": f"0x{snapshot.target_generation:016x}",
                    "ticket_lease_ms": snapshot.ticket_lease_ms,
                    "ticket_release": release.state,
                }
            )
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
