from __future__ import annotations

import dataclasses
import errno
import struct
import tempfile
import unittest
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

from vdscreen.trace import (
    InputState,
    NEUTRAL_INPUT,
    TRACE_EVENT_CHECKPOINT,
    TRACE_EVENT_INPUT,
    TRACE_EVENT_SIZE,
    TRACE_HEADER_SIZE,
    TRACE_NO_FRAME,
    TraceCleanupError,
    TraceError,
    TraceIdentity,
    TraceReplayCancelled,
    _fsync_parent_directory,
    replay_trace,
    save_trace,
    trace_listing,
    verify_trace,
)

HEADER = struct.Struct(">4sHHHHI9s3xIQQQIIQI20x")
EVENT = struct.Struct(">IHHQQIhhhhBB2xHHHH4xHHHH4x")
IDENTITY = TraceIdentity("VDSCRN001", 42, 7, 0x1122334455667788)


def _checksum(data: bytes) -> int:
    return zlib.crc32(data[76:], zlib.crc32(data[:72]))


def _trace(*, second_time: int = 1000, end_reason: int = 1) -> bytes:
    events = b"".join((
        EVENT.pack(
            1, TRACE_EVENT_INPUT, TRACE_EVENT_SIZE, 0, 10,
            0x4020, -32768, 32767, -12, 34, 2, 0,
            1, 100, 200, 300, 2, 400, 500, 600,
        ),
        EVENT.pack(
            2, TRACE_EVENT_CHECKPOINT, TRACE_EVENT_SIZE, second_time, 11,
            0, 0, 0, 0, 0, 0, 1,
            0, 0, 0, 0, 0, 0, 0, 0,
        ),
    ))
    size = TRACE_HEADER_SIZE + len(events)
    header = HEADER.pack(
        b"VDTR", 1, TRACE_HEADER_SIZE, TRACE_EVENT_SIZE, end_reason, 1,
        b"VDSCRN001", 42, 7, 0x1122334455667788,
        0x8877665544332211, 2, size, second_time, 0,
    )
    data = bytearray(header + events)
    struct.pack_into(">I", data, 72, _checksum(data))
    return bytes(data)


class FakeClock:
    def __init__(self) -> None:
        self.value = 10.0
        self.sleeps: list[float] = []

    def now(self) -> float:
        return self.value

    def sleep(self, duration: float) -> None:
        self.sleeps.append(duration)
        self.value += duration


class InputTraceTests(unittest.TestCase):
    def test_verify_list_and_atomic_save(self) -> None:
        data = _trace()
        trace = verify_trace(data, expected_identity=IDENTITY)
        self.assertEqual(trace.duration_us, 1000)
        self.assertEqual(trace.events[0].frame_index, 10)
        self.assertEqual(trace.events[0].input.buttons, 0x4020)
        self.assertEqual(trace.events[0].input.left_x, -32768)
        self.assertEqual(
            trace.events[0].input.touches,
            ((1, 100, 200, 300), (2, 400, 500, 600)),
        )
        self.assertEqual(trace.events[1].marker, 1)
        listing = trace_listing(trace)
        self.assertEqual(listing["event_count"], 2)
        self.assertEqual(listing["events"][1]["kind"], "checkpoint")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "saved.vdtrace"
            with mock.patch(
                    "vdscreen.trace._fsync_parent_directory",
                    wraps=_fsync_parent_directory) as fsync_directory:
                saved = save_trace(
                    data, output, expected_identity=IDENTITY)
            fsync_directory.assert_called_once_with(output.parent)
            self.assertEqual(saved, trace)
            self.assertEqual(output.read_bytes(), data)
            self.assertFalse((Path(directory) / "saved.vdtrace.tmp").exists())

    def test_parent_directory_fsync_has_bounded_platform_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch(
                    "vdscreen.trace.os.open",
                    side_effect=OSError(
                        errno.EINVAL, "directory fsync unsupported")):
                self.assertFalse(_fsync_parent_directory(root))
            with mock.patch(
                    "vdscreen.trace.os.open",
                    side_effect=OSError(
                        errno.EIO, "directory fsync failed")):
                with self.assertRaises(OSError):
                    _fsync_parent_directory(root)

    def test_wrong_identity_checksum_truncation_and_nonmonotonic_fail(self) -> None:
        data = _trace()
        with self.assertRaisesRegex(TraceError, "identity"):
            verify_trace(
                data,
                expected_identity=TraceIdentity(
                    "OTHER0001", 42, 7, 0x1122334455667788),
            )
        with self.assertRaisesRegex(TraceError, "identity"):
            verify_trace(
                data,
                expected_identity=TraceIdentity(
                    "VDSCRN001", 42, 8, 0x1122334455667788),
            )
        with self.assertRaisesRegex(TraceError, "identity"):
            verify_trace(
                data,
                expected_identity=TraceIdentity(
                    "VDSCRN001", 42, 7, 0x1122334455667789),
            )
        changed = bytearray(data)
        changed[TRACE_HEADER_SIZE + 24] ^= 1
        with self.assertRaisesRegex(TraceError, "checksum"):
            verify_trace(bytes(changed))
        with self.assertRaises(TraceError):
            verify_trace(data[:-1])

        changed = bytearray(_trace(second_time=1000))
        struct.pack_into(">Q", changed, TRACE_HEADER_SIZE + 8, 1000)
        struct.pack_into(">Q", changed,
                         TRACE_HEADER_SIZE + TRACE_EVENT_SIZE + 8, 500)
        struct.pack_into(">I", changed, 72, 0)
        struct.pack_into(">I", changed, 72, _checksum(changed))
        with self.assertRaisesRegex(TraceError, "time regressed"):
            verify_trace(bytes(changed))

    def test_checksum_correct_reserved_bytes_fail_closed(self) -> None:
        for offset in (25, 27, 76, 95, TRACE_HEADER_SIZE + 38,
                       TRACE_HEADER_SIZE + 51,
                       TRACE_HEADER_SIZE + 63):
            changed = bytearray(_trace())
            changed[offset] = 1
            struct.pack_into(">I", changed, 72, 0)
            struct.pack_into(">I", changed, 72, _checksum(changed))
            with self.subTest(offset=offset), self.assertRaisesRegex(
                    TraceError, "reserved"):
                verify_trace(bytes(changed))

    def test_deterministic_malformed_corpus_fails_closed(self) -> None:
        data = _trace()
        for length in range(TRACE_HEADER_SIZE):
            with self.subTest(length=length), self.assertRaises(TraceError):
                verify_trace(data[:length])
        for index in range(0, len(data), 7):
            changed = bytearray(data)
            changed[index] ^= (index * 17 + 1) & 0xFF
            with self.subTest(index=index), self.assertRaises(TraceError):
                verify_trace(bytes(changed))

    def test_cooperative_replay_is_bounded_and_neutralizes(self) -> None:
        trace = verify_trace(_trace())
        clock = FakeClock()
        applied: list[InputState] = []
        stats = replay_trace(
            trace, lambda state: applied.append(state),
            expected_identity=IDENTITY,
            now=clock.now, sleep=clock.sleep,
        )
        self.assertEqual(stats.events_dispatched, 1)
        self.assertEqual(stats.checkpoints_seen, 1)
        self.assertEqual(applied[-1], NEUTRAL_INPUT)
        self.assertEqual(applied[0].touches[1], (2, 400, 500, 600))
        self.assertTrue(clock.sleeps)
        self.assertLessEqual(max(clock.sleeps), 0.05)

    def test_replay_cancel_callback_failure_and_drift_neutralize(self) -> None:
        trace = verify_trace(_trace())
        applied: list[InputState] = []
        with self.assertRaises(TraceReplayCancelled):
            replay_trace(
                trace, lambda state: applied.append(state),
                expected_identity=IDENTITY,
                cancelled=lambda: True,
            )
        self.assertEqual(applied, [NEUTRAL_INPUT])

        applied.clear()
        with self.assertRaisesRegex(TraceError, "callback failed"):
            replay_trace(
                trace,
                lambda state: (
                    applied.append(state),
                    -1 if state != NEUTRAL_INPUT else 0,
                )[1],
                expected_identity=IDENTITY,
            )
        self.assertEqual(applied[-1], NEUTRAL_INPUT)

        applied.clear()
        with self.assertRaises(TraceCleanupError) as raised:
            replay_trace(
                trace,
                lambda state: (applied.append(state), -1)[1],
                expected_identity=IDENTITY,
            )
        self.assertIn("callback failed", str(raised.exception.primary_error))
        self.assertIn("neutral input release failed",
                      str(raised.exception.cleanup_error))
        self.assertEqual(applied[-1], NEUTRAL_INPUT)

        applied.clear()
        clock = FakeClock()

        def oversleep(duration: float) -> None:
            clock.value += duration + 0.2

        with self.assertRaisesRegex(TraceError, "drift"):
            replay_trace(
                trace, lambda state: applied.append(state),
                expected_identity=IDENTITY,
                now=clock.now, sleep=oversleep,
                max_drift_seconds=0.1,
            )
        self.assertEqual(applied[-1], NEUTRAL_INPUT)

    def test_aborted_trace_cannot_replay(self) -> None:
        trace = verify_trace(_trace(end_reason=2))
        applied: list[InputState] = []
        with self.assertRaisesRegex(TraceError, "complete"):
            replay_trace(
                trace, lambda state: applied.append(state),
                expected_identity=IDENTITY)
        self.assertEqual(applied, [])

    def test_replay_revalidates_bytes_and_live_identity(self) -> None:
        trace = verify_trace(_trace())
        applied: list[InputState] = []
        stale = dataclasses.replace(
            IDENTITY, process_generation=IDENTITY.process_generation + 1)
        with self.assertRaisesRegex(TraceError, "identity"):
            replay_trace(
                trace, lambda state: applied.append(state),
                expected_identity=stale)
        self.assertEqual(applied, [])

        malformed = bytearray(trace.data)
        malformed[TRACE_HEADER_SIZE + 24] ^= 1
        constructed = dataclasses.replace(trace, data=bytes(malformed))
        with self.assertRaisesRegex(TraceError, "checksum"):
            replay_trace(
                constructed, lambda state: applied.append(state),
                expected_identity=IDENTITY)
        self.assertEqual(applied, [])

    def test_atomic_save_owns_only_unique_temps(self) -> None:
        data = _trace()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "trace.vdtrace"
            predictable = root / "trace.vdtrace.tmp"
            predictable.write_bytes(b"unrelated")
            victim = root / "victim"
            victim.write_bytes(b"keep")
            symlink = root / ".trace.vdtrace.attacker.tmp"
            try:
                symlink.symlink_to(victim)
            except OSError:
                symlink = None
            else:
                if not symlink.is_symlink():
                    symlink.unlink()
                    symlink = None

            with ThreadPoolExecutor(max_workers=4) as executor:
                results = list(executor.map(
                    lambda _: save_trace(data, destination), range(8)))
            self.assertTrue(all(result.data == data for result in results))
            self.assertEqual(destination.read_bytes(), data)
            self.assertEqual(predictable.read_bytes(), b"unrelated")
            self.assertEqual(victim.read_bytes(), b"keep")
            if symlink is not None:
                self.assertTrue(symlink.is_symlink())
            owned_temps = list(root.glob(".trace.vdtrace.*.tmp"))
            self.assertEqual(
                owned_temps, [symlink] if symlink is not None else [])


if __name__ == "__main__":
    unittest.main()
