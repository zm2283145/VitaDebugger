#!/usr/bin/env python3
"""Decode one or two VitaDebugger DIP-switch probe journal slots."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

MAGIC = 0x56444450
VERSION = 1
SIZE = 128
FORMAT = "<7Ii4I2i18I"


def checksum(data: bytes) -> int:
    mutable = bytearray(data)
    mutable[12:16] = b"\0\0\0\0"
    value = 2166136261
    for byte in mutable:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def parse(path: Path) -> tuple[int, tuple[int, ...]]:
    data = path.read_bytes()
    if len(data) != SIZE:
        raise ValueError(f"wrong size: {len(data)} (expected {SIZE})")
    values = struct.unpack(FORMAT, data)
    magic, version, size, stored_checksum = values[:4]
    if (magic, version, size) != (MAGIC, VERSION, SIZE):
        raise ValueError("record header is invalid")
    if stored_checksum != checksum(data):
        raise ValueError("record checksum is invalid")
    return values[4], values


def newer(left: int, right: int) -> bool:
    difference = (left - right) & 0xFFFFFFFF
    return difference != 0 and difference < 0x80000000


def select_newest(
    valid: list[tuple[Path, int, tuple[int, ...]]],
) -> tuple[Path, int, tuple[int, ...]]:
    selected = valid[0]
    for candidate in valid[1:]:
        if candidate[1] == selected[1]:
            raise ValueError(
                "valid journal slots have the same revision; refusing conflict"
            )
        if newer(candidate[1], selected[1]):
            selected = candidate
    return selected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("records", nargs="+", type=Path)
    args = parser.parse_args()
    if len(args.records) > 2:
        raise SystemExit("provide at most two journal slots")

    valid: list[tuple[Path, int, tuple[int, ...]]] = []
    for path in args.records:
        try:
            revision, values = parse(path)
        except (OSError, ValueError) as error:
            print(f"{path}: invalid: {error}")
        else:
            print(f"{path}: valid revision {revision}")
            valid.append((path, revision, values))
    if not valid:
        raise SystemExit("no valid journal slot")

    try:
        path, _revision, values = select_newest(valid)
    except ValueError as error:
        raise SystemExit(str(error)) from error

    (
        _magic,
        _version,
        _size,
        _checksum,
        revision,
        sequence,
        state,
        result,
        flags,
        cp,
        debug,
        system,
        bit203,
        bit228,
        core,
        *_reserved,
    ) = values
    print(f"newest={path} revision={revision} sequence={sequence}")
    print(f"state={state} result={result} flags=0x{flags:08X} core={core}")
    print(
        f"cp_version=0x{cp & 0xFFFF:04X} "
        f"cp_build_id=0x{cp >> 16:04X} raw=0x{cp:08X}"
    )
    print(f"debug=0x{debug:08X} system=0x{system:08X}")
    print(f"bit203={bit203} bit228={bit228}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
