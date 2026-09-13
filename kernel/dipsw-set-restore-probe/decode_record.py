#!/usr/bin/env python3
"""Decode one or two VitaDebugger DIP 228 set/restore journal slots."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

MAGIC = 0x56445352
VERSION = 1
SIZE = 192
FORMAT = "<7I2i4I2i3I2iIi3I2i4Ii16I"

STATES = {
    1: "ATTEMPTED",
    2: "KERNEL_ENTERED",
    3: "ORIGINAL_CAPTURED",
    4: "SET_PENDING",
    5: "COMPLETE",
}


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
        restore_result,
        flags,
        before_cp,
        before_debug,
        before_system,
        before_203,
        before_228,
        confirm_cp,
        confirm_debug,
        confirm_system,
        confirm_203,
        confirm_228,
        set_system,
        set_228,
        restore_cp,
        restore_debug,
        restore_system,
        restore_203,
        restore_228,
        core,
        hazard_armed,
        set_calls,
        clear_calls,
        journal_error,
        *_reserved,
    ) = values
    print(f"newest={path} revision={revision} sequence={sequence}")
    print(
        f"state={STATES.get(state, f'UNKNOWN({state})')} "
        f"primary={result} restore={restore_result} "
        f"flags=0x{flags:08X} core={core}"
    )
    print(
        f"cp_version=0x{before_cp & 0xFFFF:04X} "
        f"cp_build_id=0x{before_cp >> 16:04X} raw=0x{before_cp:08X}"
    )
    print(
        f"hazard={hazard_armed} calls=set:{set_calls} clear:{clear_calls} "
        f"journal_error={journal_error}"
    )
    print(
        f"before  debug=0x{before_debug:08X} system=0x{before_system:08X} "
        f"bit203={before_203} bit228={before_228}"
    )
    print(
        f"confirm cp=0x{confirm_cp:08X} debug=0x{confirm_debug:08X} "
        f"system=0x{confirm_system:08X} "
        f"bit203={confirm_203} bit228={confirm_228}"
    )
    print(
        f"set     system=0x{set_system:08X} bit228={set_228}"
    )
    print(
        f"restore cp=0x{restore_cp:08X} debug=0x{restore_debug:08X} "
        f"system=0x{restore_system:08X} "
        f"bit203={restore_203} bit228={restore_228}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
