"""Exact durable journal namespaces for the VDCP00013 cleanup gate."""

from __future__ import annotations

JOURNAL_DIRECTORY = "VitaDebugger"
JOURNAL_PREFIX = "pmu-cleanup-v2"
DEVICE_ROOT = f"ux0:data/{JOURNAL_DIRECTORY}"
FTP_ROOT = f"ux0:/data/{JOURNAL_DIRECTORY}"
DEVICE_PATH_CAPACITY = 96
SLOTS = ("a", "b", "c")
STAGE_NAMESPACES = (
    "conflict-r2",
    "timeout",
    "disconnect",
    "normal-exit",
    "abrupt-exit",
)
HISTORICAL_STAGE1_NAMESPACE = "conflict"


def _stage_namespace(stage: int) -> str:
    if type(stage) is not int or stage not in range(1, 6):
        raise ValueError("stage must be exactly 1, 2, 3, 4, or 5")
    return STAGE_NAMESPACES[stage - 1]


def _slot_name(slot: str) -> str:
    if slot not in SLOTS:
        raise ValueError("slot must be exactly a, b, or c")
    return slot


def journal_filename(stage: int, slot: str) -> str:
    return (
        f"{JOURNAL_PREFIX}-{_stage_namespace(stage)}-"
        f"{_slot_name(slot)}.bin"
    )


def device_journal_path(stage: int, slot: str) -> str:
    return f"{DEVICE_ROOT}/{journal_filename(stage, slot)}"


def ftp_journal_path(stage: int, slot: str) -> str:
    return f"{FTP_ROOT}/{journal_filename(stage, slot)}"


def historical_stage1_filename(slot: str) -> str:
    return (
        f"{JOURNAL_PREFIX}-{HISTORICAL_STAGE1_NAMESPACE}-"
        f"{_slot_name(slot)}.bin"
    )


_ALL_DEVICE_PATHS = tuple(
    device_journal_path(stage, slot)
    for stage in range(1, 6)
    for slot in SLOTS
)
if len(set(_ALL_DEVICE_PATHS)) != len(_ALL_DEVICE_PATHS):
    raise RuntimeError("PMU cleanup journal paths must be globally unique")
if any(
    len(path.encode("ascii")) >= DEVICE_PATH_CAPACITY
    for path in _ALL_DEVICE_PATHS
):
    raise RuntimeError("PMU cleanup journal path exceeds C storage")
if set(journal_filename(1, slot) for slot in SLOTS) & set(
    historical_stage1_filename(slot) for slot in SLOTS
):
    raise RuntimeError("stage-1 retry journals collide with historical slots")
