#!/usr/bin/env python3
"""Decode and validate one VDCP00013 PMU cleanup gate journal."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

try:
    from tools.pmu_cleanup_journal_paths import journal_filename
except ModuleNotFoundError:
    from pmu_cleanup_journal_paths import journal_filename

MAGIC = 0x56504347
VERSION = 2
SIZE = 1024
NOT_RUN = -799
VP_ERROR_IO = -13
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
VP_ERROR_BUSY = -42
VITA_SYSCALL_VP_ERROR_BUSY = -1073741866
RESULT_OPEN = 0
RESULT_READ = 1
RESULT_CLOSE = 2
RESULT_AUX_CREATE = 3
RESULT_AUX_START = 4
RESULT_AUX_WAIT = 5
RESULT_AUX_DELETE = 6
RESULT_AUX_ACTION = 7
RESULT_AUX_CLEANUP = 8
RESULT_REARM_OPEN = 9
RESULT_REARM_READ = 10
RESULT_REARM_CLOSE = 11
RESULT_NET_START = 14
RESULT_NET_CONNECT = 15
RESULT_NET_PRELUDE = 16
RESULT_NET_FAILURE = 17
RESULT_NET_CLOSE = 18
RESULT_NET_STOP = 19
RESULT_POST_DISCONNECT_READ = 21
RESULTS_OFFSET = 68
HANDLES_OFFSET = 164
HANDLE_SIZE = 40
SAMPLE_PADDING_OFFSET = 284
SAMPLES_OFFSET = 288
SAMPLE_SIZE = 48
REARM_ELAPSED_OFFSET = 432
BASELINE_OFFSET = 440
RESTORED_OFFSET = 624
FINAL_OFFSET = 808
RESERVED_OFFSET = 992
REARM_DEADLINE_US = 2_000_000


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


def _handle(data: bytes, offset: int) -> dict[str, int]:
    words = struct.unpack_from("<10I", data, offset)
    return {
        "struct_size": words[0],
        "abi_version": words[1],
        "owner_token": words[2],
        "generation": words[3],
        "event_code": words[4],
        "lease_ms": words[5],
        "lease_token_low": words[6],
        "lease_token_high": words[7],
    }


def _sample(data: bytes, offset: int) -> dict[str, int]:
    words = struct.unpack_from("<8IQ2I", data, offset)
    return {
        "struct_size": words[0],
        "abi_version": words[1],
        "owner_token": words[2],
        "generation": words[3],
        "event_code": words[4],
        "core_id": words[5],
        "physical_counter": words[6],
        "flags": words[7],
        "value": words[8],
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
        "active_process_normal_exit_cleanup_count": words[13],
        "active_process_kill_cleanup_count": words[14],
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
        and status["abi_version"] == 2
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


def _busy_result(result: int) -> bool:
    return result in (VP_ERROR_BUSY, VITA_SYSCALL_VP_ERROR_BUSY)


def _sample_matches_handle(
    sample: dict[str, int],
    handle: dict[str, int],
    event_code: int,
) -> bool:
    return (
        handle["owner_token"] != 0
        and handle["generation"] != 0
        and sample["struct_size"] == 48
        and sample["abi_version"] == 1
        and sample["owner_token"] == handle["owner_token"]
        and sample["generation"] == handle["generation"]
        and sample["event_code"] == event_code
        and sample["core_id"] == 0
        and sample["physical_counter"] == 5
    )


def decode_record(
    data: bytes,
    *,
    source_journal_name: str | None = None,
    expected_stage: int | None = None,
    expected_slot: str | None = None,
) -> dict[str, object]:
    if len(data) != SIZE:
        raise ValueError(f"expected {SIZE} bytes, got {len(data)}")
    provenance_values = (
        source_journal_name,
        expected_stage,
        expected_slot,
    )
    if any(value is not None for value in provenance_values) and not all(
        value is not None for value in provenance_values
    ):
        raise ValueError(
            "source journal name, expected stage, and expected slot "
            "must be provided together"
        )
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
        "results": list(struct.unpack_from("<24i", data, RESULTS_OFFSET)),
        "handles": [
            _handle(data, HANDLES_OFFSET + index * HANDLE_SIZE)
            for index in range(3)
        ],
        "samples": [
            _sample(data, SAMPLES_OFFSET + index * SAMPLE_SIZE)
            for index in range(3)
        ],
        "handles_hex": [
            data[
                HANDLES_OFFSET + index * HANDLE_SIZE :
                HANDLES_OFFSET + (index + 1) * HANDLE_SIZE
            ].hex()
            for index in range(3)
        ],
        "samples_hex": [
            data[
                SAMPLES_OFFSET + index * SAMPLE_SIZE :
                SAMPLES_OFFSET + (index + 1) * SAMPLE_SIZE
            ].hex()
            for index in range(3)
        ],
        "rearm_elapsed_us": struct.unpack_from(
            "<Q", data, REARM_ELAPSED_OFFSET
        )[0],
        "baseline": _status(data, BASELINE_OFFSET),
        "restored": _status(data, RESTORED_OFFSET),
        "final": _status(data, FINAL_OFFSET),
        "file_sha256": hashlib.sha256(data).hexdigest(),
    }
    errors: list[str] = []
    if source_journal_name is not None:
        assert expected_stage is not None
        assert expected_slot is not None
        expected_name = journal_filename(expected_stage, expected_slot)
        record["journal_provenance"] = {
            "source_journal_name": source_journal_name,
            "expected_journal_name": expected_name,
            "expected_stage": expected_stage,
            "expected_slot": expected_slot,
        }
        expected_revision = ord(expected_slot) - ord("a") + 1
        expected_state = (
            STATE_ATTEMPTED
            if expected_slot == "a"
            else (
                STATE_ARMED
                if expected_slot == "b" and expected_stage >= 4
                else None
            )
        )
        if (
            source_journal_name != expected_name
            or header[6] != expected_stage
            or header[4] != expected_revision
            or (
                expected_state is not None
                and header[5] != expected_state
            )
            or (
                expected_slot == "b"
                and expected_stage <= 3
                and header[5] not in (STATE_COMPLETE, STATE_FAILED)
            )
            or (
                expected_slot == "c"
                and (
                    expected_stage <= 3
                    or header[5] not in (
                        STATE_COMPLETE,
                        STATE_FAILED,
                    )
                )
            )
        ):
            errors.append("journal_provenance")
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
    if any(data[SAMPLE_PADDING_OFFSET:SAMPLES_OFFSET]):
        errors.append("layout_padding")
    if any(data[RESERVED_OFFSET:SIZE]):
        errors.append("reserved")
    if header[5] == STATE_ATTEMPTED:
        if header[4] != 1 or header[7] != 0 or restored_result != NOT_RUN or final_result != NOT_RUN:
            errors.append("attempted_contract")
    elif header[5] == STATE_ARMED:
        results = record["results"]
        handles = record["handles"]
        assert isinstance(results, list)
        assert isinstance(handles, list)
        if (
            header[4] != 2
            or header[6] not in (4, 5)
            or header[7] != FLAG_OWNER_ARMED
            or results[0] != 0
            or results[1] != 0
            or handles[0]["owner_token"] == 0
            or handles[0]["generation"] == 0
        ):
            errors.append("armed_contract")
    elif header[5] == STATE_COMPLETE:
        baseline = record["baseline"]
        restored = record["restored"]
        final = record["final"]
        handles = record["handles"]
        samples = record["samples"]
        results = record["results"]
        assert isinstance(baseline, dict)
        assert isinstance(restored, dict)
        assert isinstance(final, dict)
        assert isinstance(handles, list)
        assert isinstance(samples, list)
        assert isinstance(results, list)
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
            or results[RESULT_REARM_OPEN] != 0
            or results[RESULT_REARM_READ] != 0
            or results[RESULT_REARM_CLOSE] != 0
            or not _sample_matches_handle(
                samples[1], handles[1], 0x01
            )
            or record["rearm_elapsed_us"] > REARM_DEADLINE_US
            or (
                header[6] == 1
                and (
                    results[RESULT_OPEN] != 0
                    or results[RESULT_READ] != 0
                    or results[RESULT_CLOSE] != 0
                    or results[RESULT_AUX_CREATE] < 0
                    or results[RESULT_AUX_START] < 0
                    or results[RESULT_AUX_WAIT] < 0
                    or results[RESULT_AUX_DELETE] < 0
                    or not _busy_result(results[RESULT_AUX_ACTION])
                    or results[RESULT_AUX_CLEANUP] != NOT_RUN
                    or not _sample_matches_handle(
                        samples[0], handles[0], 0x01
                    )
                )
            )
            or (
                header[6] == 3
                and (
                    results[RESULT_OPEN] != 0
                    or results[RESULT_READ] != 0
                    or results[RESULT_NET_START] != 0
                    or results[RESULT_NET_CONNECT] != 0
                    or results[RESULT_NET_PRELUDE] != 0
                    or results[RESULT_NET_FAILURE] != VP_ERROR_IO
                    or results[RESULT_CLOSE] != 0
                    or results[RESULT_NET_CLOSE] != 0
                    or results[RESULT_NET_STOP] != 0
                    or results[RESULT_POST_DISCONNECT_READ] != 0
                    or not _sample_matches_handle(
                        samples[0], handles[0], 0x10
                    )
                    or not _sample_matches_handle(
                        samples[2], handles[0], 0x10
                    )
                )
            )
            or (
                header[6] == 4
                and (
                    restored["active_process_normal_exit_cleanup_count"]
                    <= baseline["active_process_normal_exit_cleanup_count"]
                    or restored["active_process_kill_cleanup_count"]
                    != baseline["active_process_kill_cleanup_count"]
                    or final["active_process_normal_exit_cleanup_count"]
                    != restored["active_process_normal_exit_cleanup_count"]
                    or final["active_process_kill_cleanup_count"]
                    != restored["active_process_kill_cleanup_count"]
                )
            )
            or (
                header[6] == 5
                and (
                    restored["active_process_kill_cleanup_count"]
                    <= baseline["active_process_kill_cleanup_count"]
                    or restored["active_process_normal_exit_cleanup_count"]
                    != baseline["active_process_normal_exit_cleanup_count"]
                    or final["active_process_normal_exit_cleanup_count"]
                    != restored["active_process_normal_exit_cleanup_count"]
                    or final["active_process_kill_cleanup_count"]
                    != restored["active_process_kill_cleanup_count"]
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
    parser.add_argument("--source-journal-name")
    parser.add_argument(
        "--expected-stage", type=int, choices=range(1, 6)
    )
    parser.add_argument(
        "--expected-slot", choices=("a", "b", "c")
    )
    parser.add_argument(
        "--allow-unbound-source",
        action="store_true",
        help="decode historical evidence without accepting its journal provenance",
    )
    args = parser.parse_args()
    provenance_values = (
        args.source_journal_name,
        args.expected_stage,
        args.expected_slot,
    )
    if any(value is not None for value in provenance_values) and not all(
        value is not None for value in provenance_values
    ):
        parser.error(
            "--source-journal-name, --expected-stage, and "
            "--expected-slot must be supplied together"
        )
    if args.allow_unbound_source and all(
        value is not None for value in provenance_values
    ):
        parser.error(
            "--allow-unbound-source cannot be combined with bound provenance"
        )
    if not args.allow_unbound_source and not all(
        value is not None for value in provenance_values
    ):
        parser.error(
            "bound journal provenance is required; use "
            "--allow-unbound-source only for historical analysis"
        )
    decoded = decode_record(
        args.record.read_bytes(),
        source_journal_name=args.source_journal_name,
        expected_stage=args.expected_stage,
        expected_slot=args.expected_slot,
    )
    rendered = json.dumps(decoded, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0 if decoded["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
