#!/usr/bin/env python3
"""Decode the newest valid VitaDebugger staged read-ladder journal slot."""

from __future__ import annotations

import argparse
import pathlib
import struct

MAGIC = 0x56444C52
VERSION = 1
SIZE = 64
FORMAT = "<11I5I"
STEPS = {
    1: "kernel lifecycle/no-op",
    2: "CPU-ID API",
    3: "MIDR (CP15)",
    4: "DIDR (CP14)",
    5: "DSCR (CP14)",
    6: "DBGVCR (CP14)",
    7: "BCR0 (CP14)",
    8: "BVR0 (CP14)",
    9: "WCR0 (CP14)",
    10: "WVR0 (CP14)",
}
STATES = {1: "loader armed", 2: "kernel entered", 3: "complete"}


def checksum(blob: bytes) -> int:
    mutable = bytearray(blob)
    mutable[12:16] = b"\0\0\0\0"
    value = 2166136261
    for byte in mutable:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def load(path: pathlib.Path) -> dict[str, int] | None:
    blob = path.read_bytes()
    if len(blob) != SIZE:
        return None
    fields = struct.unpack(FORMAT, blob)
    names = (
        "magic", "version", "size", "checksum", "revision", "sequence",
        "step", "state", "result_u32", "value", "flags", "core_id",
        "reserved0", "reserved1", "reserved2", "reserved3",
    )
    record = dict(zip(names, fields, strict=True))
    if (record["magic"] != MAGIC or record["version"] != VERSION or
            record["size"] != SIZE or record["checksum"] != checksum(blob)):
        return None
    result = record["result_u32"]
    record["result"] = result if result < 0x80000000 else result - 0x100000000
    return record


def newer(left: int, right: int) -> bool:
    delta = (left - right) & 0xFFFFFFFF
    return 0 < delta < 0x80000000


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("slot_a", type=pathlib.Path)
    parser.add_argument("slot_b", type=pathlib.Path)
    args = parser.parse_args()
    records = []
    for label, path in (("A", args.slot_a), ("B", args.slot_b)):
        try:
            record = load(path)
        except OSError as error:
            print(f"slot {label}: unreadable ({error})")
            continue
        if record is None:
            print(f"slot {label}: invalid")
            continue
        records.append((label, record))
        print(f"slot {label}: valid revision={record['revision']}")
    if not records:
        print("no valid journal slot")
        return 1
    label, record = records[0]
    if len(records) == 2 and newer(records[1][1]["revision"],
                                   record["revision"]):
        label, record = records[1]
    step = record["step"]
    state = record["state"]
    print(f"newest slot: {label}")
    print(f"sequence: {record['sequence']}")
    print(f"step: {step} ({STEPS.get(step, 'invalid')})")
    print(f"state: {state} ({STATES.get(state, 'unknown')})")
    print(f"result: {record['result']}")
    print(f"value: 0x{record['value']:08X}")
    print(f"flags: 0x{record['flags']:08X}")
    print(f"expected core: {record['core_id']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
