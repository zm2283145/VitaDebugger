#!/usr/bin/env python3
"""Decode and validate one VDCP00013 PMU cleanup gate journal."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = 0x56504347
VERSION = 1
SIZE = 1024
NOT_RUN = -799
STATE_ATTEMPTED = 1
STATE_ARMED = 2
STATE_COMPLETE = 3
STATE_FAILED = 4
FLAG_ACTION = 1 << 0
FLAG_RESTORED = 1 << 1
FLAG_REARMED = 1 << 2
FLAG_OWNER_ARMED = 1 << 3
FLAG_PASS = 1 << 4
COMPLETE_FLAGS = FLAG_ACTION | FLAG_RESTORED | FLAG_REARMED | FLAG_PASS
REQUIRED_CAPABILITIES = 0x6B


def _fnv1a(data: bytes) -> int:
    value = 2166136261
    for index, byte in enumerate(data):
        value ^= 0 if 12 <= index < 16 else byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def _snapshot(data: bytes, offset: int) -> dict[str, object]:
    words = struct.unpack_from("<20I", data, offset)
    return {
        "event_counter_count": words[0],
        "raw_pmcr": words[1],
        "raw_pmcntenset": words[2],
        "raw_pmovsr": words[3],
        "raw_pmselr": words[4],
        "raw_pmccntr": words[5],
        "raw_pmuserenr": words[6],
        "raw_pmintenset": words[7],
        "raw_pmxevtyper": list(words[8:14]),
        "raw_pmxevcntr": list(words[14:20]),
    }


def _status(data: bytes, offset: int) -> dict[str, object]:
    words = struct.unpack_from("<21I", data, offset)
    return {
        "struct_size": words[0],
        "abi_version": words[1],
        "transport_state": words[2],
        "last_result": struct.unpack_from("<i", data, offset + 12)[0],
        "owner_pid": struct.unpack_from("<i", data, offset + 16)[0],
        "owner_thread": struct.unpack_from("<i", data, offset + 20)[0],
        "owner_token": words[6],
        "generation": words[7],
        "real_event_attempted": words[8],
        "exact_restore_proven": words[9],
        "owner_identity_valid": words[10],
        "owner_identity_release_uncertain": words[11],
        "rearm_count": words[12],
        "process_normal_exit_cleanup_count": words[13],
        "process_kill_cleanup_count": words[14],
        "owner_process_terminal_kind": words[15],
        "owner_terminal_reference_released": words[16],
        "backend_ready": words[17],
        "backend_recovery_pending": words[18],
        "backend_restore_obligation": words[19],
        "snapshot_result": struct.unpack_from("<i", data, offset + 80)[0],
        "snapshot": _snapshot(data, offset + 84),
        "reserved": list(struct.unpack_from("<5I", data, offset + 164)),
    }


def _status_idle(status: dict[str, object]) -> bool:
    snapshot = status["snapshot"]
    assert isinstance(snapshot, dict)
    return (
        status["struct_size"] == 184
        and status["abi_version"] == 1
        and status["transport_state"] == 0
        and status["owner_pid"] == -1
        and status["owner_thread"] == -1
        and status["owner_token"] == 0
        and status["generation"] == 0
        and status["owner_identity_valid"] == 0
        and status["owner_identity_release_uncertain"] == 0
        and status["owner_process_terminal_kind"] == 0
        and status["owner_terminal_reference_released"] == 0
        and status["backend_ready"] == 1
        and status["backend_recovery_pending"] == 0
        and status["backend_restore_obligation"] == 0
        and status["snapshot_result"] == 0
        and snapshot["event_counter_count"] == 6
        and not any(status["reserved"])
    )


def decode_record(data: bytes) -> dict[str, object]:
    if len(data) != SIZE:
        raise ValueError(f"expected {SIZE} bytes, got {len(data)}")
    header = struct.unpack_from("<8I2Q", data, 0)
    info_result, capabilities, baseline_result, restored_result, final_result = (
        struct.unpack_from("<iIiii", data, 48)
    )
    record = {
        "title_id": "VDCP00013",
        "magic": header[0],
        "version": header[1],
        "size": header[2],
        "checksum": header[3],
        "revision": header[4],
        "state": header[5],
        "stage": header[6],
        "flags": header[7],
        "started_us": header[8],
        "finished_us": header[9],
        "info_result": info_result,
        "capabilities": capabilities,
        "baseline_status_result": baseline_result,
        "restored_status_result": restored_result,
        "final_status_result": final_result,
        "results": list(struct.unpack_from("<24i", data, 68)),
        "handles_hex": [
            data[164 + index * 40 : 204 + index * 40].hex()
            for index in range(3)
        ],
        "samples_hex": [
            data[284 + index * 48 : 332 + index * 48].hex()
            for index in range(3)
        ],
        "rearm_elapsed_us": struct.unpack_from("<Q", data, 432)[0],
        "baseline": _status(data, 440),
        "restored": _status(data, 624),
        "final": _status(data, 808),
        "file_sha256": hashlib.sha256(data).hexdigest(),
    }
    errors: list[str] = []
    if header[0] != MAGIC or header[1] != VERSION or header[2] != SIZE:
        errors.append("header")
    if header[3] != _fnv1a(data):
        errors.append("checksum")
    if header[4] not in (1, 2, 3) or header[5] not in range(1, 5):
        errors.append("state")
    if header[6] not in range(1, 6) or header[7] & ~0x1F:
        errors.append("stage_or_flags")
    if info_result != 0 or capabilities & REQUIRED_CAPABILITIES != REQUIRED_CAPABILITIES:
        errors.append("capabilities")
    if baseline_result != 0 or not _status_idle(record["baseline"]):
        errors.append("baseline")
    if any(data[992:1024]):
        errors.append("reserved")
    if header[5] == STATE_ATTEMPTED:
        if header[4] != 1 or header[7] != 0 or restored_result != NOT_RUN or final_result != NOT_RUN:
            errors.append("attempted_contract")
    elif header[5] == STATE_ARMED:
        results = record["results"]
        assert isinstance(results, list)
        if (
            header[4] != 2
            or header[6] not in (4, 5)
            or header[7] != FLAG_OWNER_ARMED
            or results[0] != 0
            or results[1] != 0
        ):
            errors.append("armed_contract")
    elif header[5] == STATE_COMPLETE:
        baseline = record["baseline"]
        restored = record["restored"]
        final = record["final"]
        assert isinstance(baseline, dict)
        assert isinstance(restored, dict)
        assert isinstance(final, dict)
        if (
            header[4] != (3 if header[6] >= 4 else 2)
            or header[7] != COMPLETE_FLAGS
            or restored_result != 0
            or final_result != 0
            or not _status_idle(restored)
            or not _status_idle(final)
            or restored["snapshot"] != baseline["snapshot"]
            or final["snapshot"] != baseline["snapshot"]
            or final["rearm_count"] <= baseline["rearm_count"]
            or (
                header[6] == 4
                and (
                    restored["process_normal_exit_cleanup_count"]
                    <= baseline["process_normal_exit_cleanup_count"]
                    or restored["process_kill_cleanup_count"]
                    != baseline["process_kill_cleanup_count"]
                    or final["process_normal_exit_cleanup_count"]
                    != restored["process_normal_exit_cleanup_count"]
                    or final["process_kill_cleanup_count"]
                    != restored["process_kill_cleanup_count"]
                )
            )
            or (
                header[6] == 5
                and (
                    restored["process_kill_cleanup_count"]
                    <= baseline["process_kill_cleanup_count"]
                    or restored["process_normal_exit_cleanup_count"]
                    != baseline["process_normal_exit_cleanup_count"]
                    or final["process_normal_exit_cleanup_count"]
                    != restored["process_normal_exit_cleanup_count"]
                    or final["process_kill_cleanup_count"]
                    != restored["process_kill_cleanup_count"]
                )
            )
        ):
            errors.append("completion_contract")
    elif header[7] & FLAG_PASS:
        errors.append("failed_contract")
    record["valid"] = not errors
    record["validation_errors"] = errors
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("record", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    decoded = decode_record(args.record.read_bytes())
    rendered = json.dumps(decoded, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0 if decoded["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
