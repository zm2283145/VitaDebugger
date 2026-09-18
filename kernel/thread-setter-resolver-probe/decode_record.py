#!/usr/bin/env python3
"""Validate and decode the fixed VitaDebugger ThreadMgr resolver journal."""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

MAGIC = 0x56545052
VERSION = 2
SIZE = 640
SEGMENT_COUNT = 4
TARGET_COUNT = 4
CODE_BYTES = 64
EXPORT_MAX_BYTES = 65536
ANY_LIBRARY = 0xFFFFFFFF
NOT_ATTEMPTED = -0x80000000
NOT_RUN = -100
EXECUTE_PERMISSION = 1

STATE_ATTEMPTED = 1
STATE_KERNEL_ENTERED = 2
STATE_FIRMWARE_RECORDED = 3
STATE_MODULE_RECORDED = 4
STATE_TARGET_RECORDED = 5
STATE_COMPLETE = 6

RESULT_NAMES = {
    0: "PASS",
    -100: "not run",
    -1: "module lookup failed",
    -3: "module identity mismatch",
    -5: "one or more exports missing",
}
STATE_NAMES = {
    STATE_ATTEMPTED: "loader armed",
    STATE_KERNEL_ENTERED: "kernel entered",
    STATE_FIRMWARE_RECORDED: "firmware recorded",
    STATE_MODULE_RECORDED: "module recorded",
    STATE_TARGET_RECORDED: "target recorded",
    STATE_COMPLETE: "complete",
}

FLAG_FIRMWARE_QUERY_OK = 1 << 0
FLAG_MODULE_LOOKUP_OK = 1 << 1
FLAG_MODULE_INFO_OK = 1 << 2
FLAG_MODULE_NAME_OK = 1 << 3
FLAG_EXPORTS_BOUNDED = 1 << 4
FLAG_ALL_RESOLVED = 1 << 5
FLAG_ALL_EXECUTABLE = 1 << 6
FLAG_ALL_CAPTURED = 1 << 7
FLAG_COMPLETE = 1 << 8
RECORD_FLAGS_ALLOWED = (
    FLAG_FIRMWARE_QUERY_OK
    | FLAG_MODULE_LOOKUP_OK
    | FLAG_MODULE_NAME_OK
    | FLAG_ALL_RESOLVED
    | FLAG_COMPLETE
)

TARGET_RESOLVED = 1 << 0
TARGET_THUMB = 1 << 1
TARGET_IN_SEGMENT = 1 << 2
TARGET_EXECUTABLE = 1 << 3
TARGET_WINDOW_BOUNDED = 1 << 4
TARGET_CAPTURED = 1 << 5
TARGET_FLAGS_ALLOWED = TARGET_RESOLVED | TARGET_THUMB

EXPECTED_MODULE = "SceKernelThreadMgr"
EXPECTED_TARGETS = (
    (1, 0x5022689D, "core-get"),
    (2, 0x64E89DE9, "core-set"),
    (3, 0x5CDE387A, "vfp-get"),
    (4, 0x49A0B679, "vfp-set"),
)


@dataclass(frozen=True)
class Segment:
    base: int
    memsz: int
    filesz: int
    permissions: int


@dataclass(frozen=True)
class Target:
    kind: int
    nid: int
    lookup_result: int
    flags: int
    raw_address: int
    code_address: int
    segment_index: int
    segment_offset: int
    code_size: int
    code: bytes


@dataclass(frozen=True)
class Record:
    path: Path
    revision: int
    sequence: int
    state: int
    result: int
    flags: int
    firmware_result: int
    firmware_version: int
    module_lookup_result: int
    module_info_result: int
    module_id: int
    module_nid: int
    exports_start: int
    exports_end: int
    module_name: str
    segment_count: int
    resolved_count: int
    executable_count: int
    captured_count: int
    lookup_library_nid: int
    completed_target_count: int
    segments: tuple[Segment, ...]
    targets: tuple[Target, ...]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def checksum(data: bytes) -> int:
    if len(data) != SIZE:
        raise ValueError(f"wrong size: {len(data)} (expected {SIZE})")
    mutable = bytearray(data)
    mutable[12:16] = b"\0\0\0\0"
    value = 2166136261
    for byte in mutable:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def range_within(start: int, size: int, base: int, span: int) -> bool:
    if size == 0 or span == 0 or start < base:
        return False
    offset = start - base
    return offset < span and size <= span - offset


def exports_bounded(record: Record) -> bool:
    start, end = record.exports_start, record.exports_end
    if start == 0 or end <= start or end - start > EXPORT_MAX_BYTES:
        return False
    return any(
        range_within(start, end - start, segment.base, segment.memsz)
        for segment in record.segments
    )


def expected_result(record: Record) -> int:
    if record.module_lookup_result < 0:
        return -1
    if record.module_name != EXPECTED_MODULE:
        return -3
    if record.completed_target_count != TARGET_COUNT:
        return NOT_RUN
    if record.resolved_count != TARGET_COUNT:
        return -5
    return 0


def parse_record_bytes(data: bytes, path: Path | None = None) -> Record:
    source = path or Path("<memory>")
    if len(data) != SIZE:
        raise ValueError(f"wrong size: {len(data)} (expected {SIZE})")
    if (u32(data, 0), u32(data, 4), u32(data, 8)) != (MAGIC, VERSION, SIZE):
        raise ValueError("record header is invalid")
    if u32(data, 12) != checksum(data):
        raise ValueError("record checksum is invalid")

    name_bytes = data[68:96]
    if b"\0" not in name_bytes:
        raise ValueError("module name is not terminated")
    terminator = name_bytes.index(0)
    if any(name_bytes[terminator:]):
        raise ValueError("module name padding is nonzero")
    try:
        module_name = name_bytes[:terminator].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("module name is not ASCII") from error

    segments = tuple(
        Segment(*struct.unpack_from("<4I", data, 128 + index * 16))
        for index in range(SEGMENT_COUNT)
    )
    targets: list[Target] = []
    for index in range(TARGET_COUNT):
        offset = 192 + index * 100
        fields = struct.unpack_from("<2Ii3Ii2I", data, offset)
        targets.append(Target(*fields, data[offset + 36 : offset + 100]))

    record = Record(
        path=source,
        revision=u32(data, 16),
        sequence=u32(data, 20),
        state=u32(data, 24),
        result=i32(data, 28),
        flags=u32(data, 32),
        firmware_result=i32(data, 36),
        firmware_version=u32(data, 40),
        module_lookup_result=i32(data, 44),
        module_info_result=i32(data, 48),
        module_id=i32(data, 52),
        module_nid=u32(data, 56),
        exports_start=u32(data, 60),
        exports_end=u32(data, 64),
        module_name=module_name,
        segment_count=u32(data, 96),
        resolved_count=u32(data, 100),
        executable_count=u32(data, 104),
        captured_count=u32(data, 108),
        lookup_library_nid=u32(data, 112),
        completed_target_count=u32(data, 116),
        segments=segments,
        targets=tuple(targets),
    )
    validate_semantics(record, data)
    return record


def parse(path: Path) -> Record:
    return parse_record_bytes(path.read_bytes(), path)


def validate_semantics(record: Record, data: bytes) -> None:
    if record.lookup_library_nid != ANY_LIBRARY:
        raise ValueError("lookup library is not TAI_ANY_LIBRARY")
    if any(data[120:128]) or any(data[592:640]):
        raise ValueError("reserved bytes are nonzero")
    for target, expected in zip(record.targets, EXPECTED_TARGETS):
        if (target.kind, target.nid) != expected[:2]:
            raise ValueError("fixed target identity is invalid")

    if record.state in (STATE_ATTEMPTED, STATE_KERNEL_ENTERED):
        validate_attempt(record)
    elif record.state in (
        STATE_FIRMWARE_RECORDED,
        STATE_MODULE_RECORDED,
        STATE_TARGET_RECORDED,
    ):
        validate_progress(record)
    elif record.state == STATE_COMPLETE:
        validate_complete(record)
    else:
        raise ValueError(f"unknown state {record.state}")


def validate_attempt(record: Record) -> None:
    if (
        record.result != NOT_RUN
        or record.flags != 0
        or record.firmware_result != NOT_ATTEMPTED
        or record.firmware_version != 0
        or record.module_lookup_result != NOT_ATTEMPTED
        or record.module_info_result != NOT_ATTEMPTED
        or record.module_id != 0
        or record.module_nid != 0
        or record.exports_start != 0
        or record.exports_end != 0
        or record.module_name
        or any((record.segment_count, record.resolved_count,
                record.executable_count, record.captured_count))
        or record.completed_target_count
        or any(any(asdict(segment).values()) for segment in record.segments)
    ):
        raise ValueError("attempt payload is not empty")
    for target in record.targets:
        if (
            target.lookup_result != NOT_ATTEMPTED
            or target.flags != 0
            or target.raw_address != 0
            or target.code_address != 0
            or target.segment_index != -1
            or target.segment_offset != 0
            or target.code_size != 0
            or any(target.code)
        ):
            raise ValueError("attempt target payload is not empty")


def validate_firmware(record: Record) -> None:
    if record.firmware_result == NOT_ATTEMPTED:
        raise ValueError("firmware metadata query was not attempted")
    if (record.firmware_result >= 0 and record.firmware_version == 0) or (
        record.firmware_result < 0 and record.firmware_version != 0
    ):
        raise ValueError("firmware metadata payload is inconsistent")


def validate_module(record: Record) -> None:
    if record.module_lookup_result == NOT_ATTEMPTED:
        raise ValueError("module lookup was not attempted")
    if record.module_lookup_result < 0:
        if (
            record.module_id != 0
            or record.module_nid != 0
            or record.exports_start != 0
            or record.exports_end != 0
            or record.module_name
        ):
            raise ValueError("failed module lookup leaked partial metadata")
    elif (
        record.module_id <= 0
        or record.module_nid == 0
        or not record.module_name
    ):
        raise ValueError("successful module lookup metadata is incomplete")


def validate_targets(record: Record, completed: int) -> int:
    resolved = 0
    for index, target in enumerate(record.targets):
        attempted = index < completed
        if (target.lookup_result != NOT_ATTEMPTED) != attempted:
            raise ValueError("target lookup lifecycle is inconsistent")
        if target.flags & ~(TARGET_RESOLVED | TARGET_THUMB):
            raise ValueError("target has forbidden trust or capture flags")
        if (
            target.segment_index != -1
            or target.segment_offset
            or target.code_size
            or any(target.code)
        ):
            raise ValueError("non-dereferencing target contains code metadata")
        if not attempted or target.lookup_result < 0:
            if target.flags or target.raw_address or target.code_address:
                raise ValueError("failed target lookup contains payload")
            continue
        if (
            not (target.flags & TARGET_RESOLVED)
            or target.raw_address == 0
            or target.code_address == 0
            or target.code_address != target.raw_address & ~1
            or bool(target.raw_address & 1) != bool(target.flags & TARGET_THUMB)
        ):
            raise ValueError("resolved target address is inconsistent")
        resolved += 1
    if record.resolved_count != resolved:
        raise ValueError("target counters are inconsistent")
    return resolved


def validate_common_prerequisite(record: Record) -> None:
    if (
        record.module_info_result != NOT_ATTEMPTED
        or record.segment_count
        or record.executable_count
        or record.captured_count
        or any(any(asdict(segment).values()) for segment in record.segments)
    ):
        raise ValueError("prerequisite record contains dereference metadata")


def validate_progress(record: Record) -> None:
    if record.result != NOT_RUN or record.flags:
        raise ValueError("progress result or flags are invalid")
    validate_common_prerequisite(record)
    validate_firmware(record)
    if record.state == STATE_FIRMWARE_RECORDED:
        if (
            record.completed_target_count
            or record.module_lookup_result != NOT_ATTEMPTED
            or record.module_id
            or record.module_nid
            or record.exports_start
            or record.exports_end
            or record.module_name
        ):
            raise ValueError("firmware checkpoint contains later metadata")
    else:
        validate_module(record)
    if record.state == STATE_TARGET_RECORDED:
        if not 1 <= record.completed_target_count <= TARGET_COUNT:
            raise ValueError("target checkpoint count is invalid")
    elif record.completed_target_count:
        raise ValueError("pre-target checkpoint has a target count")
    validate_targets(record, record.completed_target_count)


def validate_complete(record: Record) -> None:
    forbidden_flags = (
        FLAG_MODULE_INFO_OK
        | FLAG_EXPORTS_BOUNDED
        | FLAG_ALL_EXECUTABLE
        | FLAG_ALL_CAPTURED
    )
    if (
        record.flags & ~RECORD_FLAGS_ALLOWED
        or record.flags & forbidden_flags
        or not (record.flags & FLAG_COMPLETE)
        or record.completed_target_count != TARGET_COUNT
    ):
        raise ValueError("complete record flags are invalid")
    validate_common_prerequisite(record)
    validate_firmware(record)
    validate_module(record)
    resolved = validate_targets(record, TARGET_COUNT)
    checks = (
        (FLAG_ALL_RESOLVED, resolved == TARGET_COUNT),
        (FLAG_FIRMWARE_QUERY_OK, record.firmware_result >= 0),
        (FLAG_MODULE_LOOKUP_OK, record.module_lookup_result >= 0),
        (FLAG_MODULE_NAME_OK, record.module_name == EXPECTED_MODULE),
    )
    for flag, expected in checks:
        if bool(record.flags & flag) != expected:
            raise ValueError(f"record flag 0x{flag:X} is inconsistent")
    if record.result != expected_result(record):
        raise ValueError("resolver result is inconsistent")


def newer(left: int, right: int) -> bool:
    difference = (left - right) & 0xFFFFFFFF
    return difference != 0 and difference < 0x80000000


def select_newest(records: list[Record]) -> Record:
    selected = records[0]
    for candidate in records[1:]:
        if candidate.revision == selected.revision:
            raise ValueError("valid journal slots have the same revision")
        if newer(candidate.revision, selected.revision):
            selected = candidate
    return selected


def format_reported_firmware(value: int) -> str:
    if value == 0:
        return "unavailable"
    return f"{(value >> 24) & 0xFF:X}.{(value >> 16) & 0xFF:02X} (0x{value:08X})"


def fingerprint(record: Record) -> str | None:
    return None


def extract(record: Record, directory: Path, actual_baseline: str) -> None:
    if record.state != STATE_COMPLETE:
        raise ValueError("newest record is not complete")
    directory.mkdir(parents=True, exist_ok=True)
    targets_json = []
    for target, (_kind, _nid, label) in zip(record.targets, EXPECTED_TARGETS):
        mode = "thumb" if target.flags & TARGET_THUMB else "arm"
        targets_json.append(
            {
                "name": label,
                "nid": f"0x{target.nid:08X}",
                "lookup_result": target.lookup_result,
                "raw_address": f"0x{target.raw_address:08X}",
                "code_address": f"0x{target.code_address:08X}",
                "instruction_set": mode,
                "address_is_authenticated": False,
                "address_was_dereferenced": False,
                "code_size": 0,
                "code_sha256": None,
                "file": None,
            }
        )
    metadata = {
        "record_version": VERSION,
        "record_path": str(record.path),
        "revision": record.revision,
        "sequence": record.sequence,
        "result": record.result,
        "reported_firmware_raw": f"0x{record.firmware_version:08X}",
        "reported_firmware_is_spoofable": True,
        "actual_test_baseline": actual_baseline,
        "module_name": record.module_name,
        "module_id": f"0x{record.module_id & 0xFFFFFFFF:08X}",
        "module_nid": f"0x{record.module_nid:08X}",
        "exports_start": f"0x{record.exports_start:08X}",
        "exports_end": f"0x{record.exports_end:08X}",
        "lookup_library_nid": f"0x{record.lookup_library_nid:08X}",
        "segments": [
            {
                "base": f"0x{segment.base:08X}",
                "memsz": f"0x{segment.memsz:X}",
                "filesz": f"0x{segment.filesz:X}",
                "permissions": f"0x{segment.permissions:X}",
            }
            for segment in record.segments
            if any(asdict(segment).values())
        ],
        "fixed_code_fingerprint_sha256": fingerprint(record),
        "fingerprint_status": "unresolved; v2 captures no code bytes",
        "targets": targets_json,
    }
    (directory / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate and decode one or two resolver journal slots"
    )
    parser.add_argument("records", nargs="+", type=Path)
    parser.add_argument("--extract-dir", type=Path)
    parser.add_argument(
        "--actual-baseline",
        default="not supplied",
        help="human-reviewed device/firmware baseline stored in metadata",
    )
    args = parser.parse_args()
    if len(args.records) > 2:
        raise SystemExit("provide at most two journal slots")

    valid: list[Record] = []
    for path in args.records:
        try:
            record = parse(path)
        except (OSError, ValueError) as error:
            print(f"{path}: invalid: {error}")
        else:
            print(f"{path}: valid revision {record.revision}")
            valid.append(record)
    if not valid:
        raise SystemExit("no valid journal slot")
    try:
        record = select_newest(valid)
    except ValueError as error:
        raise SystemExit(str(error)) from error

    print(
        f"newest={record.path} revision={record.revision} "
        f"sequence={record.sequence}"
    )
    print(
        f"state={STATE_NAMES.get(record.state, 'unknown')} "
        f"result={RESULT_NAMES.get(record.result, 'unknown')} ({record.result}) "
        f"flags=0x{record.flags:08X}"
    )
    if record.state == STATE_COMPLETE:
        print(
            "reported_firmware="
            f"{format_reported_firmware(record.firmware_version)} "
            "[metadata only; spoofable]"
        )
        print(
            f"module={record.module_name or '-'} "
            f"id=0x{record.module_id & 0xFFFFFFFF:08X} "
            f"nid=0x{record.module_nid:08X}"
        )
        print(
            f"exports=0x{record.exports_start:08X}-0x{record.exports_end:08X} "
            f"library=0x{record.lookup_library_nid:08X}"
        )
        print(f"resolved={record.resolved_count} of {TARGET_COUNT} [presence only]")
        for target, (_kind, _nid, label) in zip(record.targets, EXPECTED_TARGETS):
            mode = "Thumb" if target.flags & TARGET_THUMB else "ARM"
            print(
                f"{label:8} nid=0x{target.nid:08X} lookup={target.lookup_result} "
                f"raw=0x{target.raw_address:08X} code=0x{target.code_address:08X} "
                f"flags=0x{target.flags:02X} mode={mode} "
                "[untrusted; not dereferenced]"
            )
        print("fixed_code_fingerprint_sha256=- [not collected]")
    elif record.state == STATE_TARGET_RECORDED:
        print(
            f"durable_target_checkpoints={record.completed_target_count}/"
            f"{TARGET_COUNT}"
        )

    if args.extract_dir:
        try:
            extract(record, args.extract_dir, args.actual_baseline)
        except ValueError as error:
            raise SystemExit(f"cannot extract: {error}") from error
        print(f"wrote non-dereferencing metadata to {args.extract_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
