import argparse
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import rsp_admission_diagnostic as diagnostic  # noqa: E402
import rsp_admission_snapshot as snapshot_transport  # noqa: E402


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


class NullTranscript:
    def event(self, *_args, **_kwargs) -> None:
        pass


class RecordingTranscript:
    def __init__(self) -> None:
        self.events = []

    def event(self, name, **fields) -> None:
        self.events.append((name, fields))


class BrokenTranscript:
    def event(self, *_args, **_kwargs) -> None:
        raise OSError("disk full")


def snapshot(*, ready: bool = False, detached: bool = False) -> dict:
    events = {
        name: {"occurrences": 0, "epoch": 0}
        for name in diagnostic.EVENT_NAMES
    }
    if ready:
        events["listener_ready"]["occurrences"] = 1
        events["listener_ready"]["epoch"] = 1
        events["test_title_ready"]["occurrences"] = 1
    if detached:
        for name in (
            "target_running",
            "protocol_released",
            "session_normalized",
            "listener_reopened",
        ):
            events[name]["occurrences"] = 1
            events[name]["epoch"] = 1
    return {
        "events": events,
        "current": {
            "listener": 7 if ready or detached else -1,
            "socket": -1 if ready or detached else 9,
            "candidate": -1,
            "owner": 0 if detached else 1,
            "owner_epoch": 0 if detached else 1,
            "connection_epoch": 1,
            "target_stopped": not detached,
            "network_closing": False,
            "test_title_ready": ready,
        },
    }


class AdmissionDiagnosticTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.transcript = NullTranscript()

    def transient_miss(self, *_args) -> dict:
        timeout = _args[2]
        self.clock.advance(timeout)
        raise diagnostic.SnapshotUnavailable("transient UDP miss")

    def test_ready_retries_transient_misses_until_success(self) -> None:
        responses = [
            self.transient_miss,
            self.transient_miss,
            lambda *_args: snapshot(ready=True),
        ]
        with (
            mock.patch.object(
                diagnostic.time, "monotonic",
                side_effect=self.clock.monotonic,
            ),
            mock.patch.object(
                diagnostic, "query_snapshot",
                side_effect=lambda *args: responses.pop(0)(*args),
            ) as query,
        ):
            result = diagnostic.wait_debugger_ready(
                "10.1.1.217", 1235, 3.0, self.transcript
            )
        self.assertTrue(result["current"]["test_title_ready"])
        self.assertEqual(query.call_count, 3)

    def test_detach_retries_transient_misses_until_success(self) -> None:
        responses = [
            self.transient_miss,
            self.transient_miss,
            lambda *_args: snapshot(detached=True),
        ]
        with (
            mock.patch.object(
                diagnostic.time, "monotonic",
                side_effect=self.clock.monotonic,
            ),
            mock.patch.object(
                diagnostic, "query_snapshot",
                side_effect=lambda *args: responses.pop(0)(*args),
            ) as query,
        ):
            result = diagnostic.wait_detached(
                "10.1.1.217", 1235, 3.0, self.transcript
            )
        self.assertLess(result["current"]["socket"], 0)
        self.assertEqual(query.call_count, 3)

    def test_ready_deadline_expires_after_transient_misses(self) -> None:
        with (
            mock.patch.object(
                diagnostic.time, "monotonic",
                side_effect=self.clock.monotonic,
            ),
            mock.patch.object(
                diagnostic, "query_snapshot",
                side_effect=self.transient_miss,
            ) as query,
        ):
            with self.assertRaisesRegex(
                diagnostic.AdmissionDiagnosticFailure,
                "before deadline.*transient UDP miss",
            ):
                diagnostic.wait_debugger_ready(
                    "10.1.1.217", 1235, 2.0, self.transcript
                )
        self.assertEqual(query.call_count, 2)

    def test_detach_deadline_expires_after_transient_misses(self) -> None:
        with (
            mock.patch.object(
                diagnostic.time, "monotonic",
                side_effect=self.clock.monotonic,
            ),
            mock.patch.object(
                diagnostic, "query_snapshot",
                side_effect=self.transient_miss,
            ) as query,
        ):
            with self.assertRaisesRegex(
                diagnostic.AdmissionDiagnosticFailure,
                "before deadline.*transient UDP miss",
            ):
                diagnostic.wait_detached(
                    "10.1.1.217", 1235, 2.0, self.transcript
                )
        self.assertEqual(query.call_count, 2)

    def test_malformed_snapshot_failure_is_not_retried(self) -> None:
        malformed = diagnostic.AdmissionDiagnosticFailure(
            "diagnostic ABI mismatch"
        )
        with mock.patch.object(
            diagnostic, "query_snapshot", side_effect=malformed
        ) as query:
            with self.assertRaisesRegex(
                diagnostic.AdmissionDiagnosticFailure,
                "diagnostic ABI mismatch",
            ):
                diagnostic.wait_debugger_ready(
                    "10.1.1.217", 1235, 3.0, self.transcript
                )
        query.assert_called_once()

    def test_transcript_io_failure_is_not_a_transient_udp_miss(self) -> None:
        client = mock.MagicMock()
        client.__enter__.return_value = client
        with (
            mock.patch.object(
                snapshot_transport.socket, "socket", return_value=client
            ),
            mock.patch.object(
                diagnostic.time, "monotonic", side_effect=[0.0, 0.0, 0.0]
            ),
        ):
            with self.assertRaisesRegex(OSError, "disk full"):
                diagnostic.query_snapshot(
                    "10.1.1.217", 1235, 1.0, BrokenTranscript()
                )
        client.sendto.assert_not_called()

    def test_query_records_each_transient_udp_miss(self) -> None:
        client = mock.MagicMock()
        client.__enter__.return_value = client
        client.recvfrom.side_effect = snapshot_transport.socket.timeout()
        transcript = RecordingTranscript()
        with (
            mock.patch.object(
                snapshot_transport.socket, "socket", return_value=client
            ),
            mock.patch.object(
                snapshot_transport.time, "monotonic",
                side_effect=[0.0, 0.0, 0.0, 1.1],
            ),
        ):
            with self.assertRaises(snapshot_transport.SnapshotUnavailable):
                snapshot_transport.query_snapshot(
                    "10.1.1.217", 1235, 1.0, transcript
                )
        self.assertEqual(
            transcript.events,
            [
                (
                    "diagnostic_query",
                    {
                        "peer": "10.1.1.217:1235",
                        "request": "UVDB-ADMISSION-1",
                    },
                ),
                (
                    "diagnostic_miss",
                    {"peer": "10.1.1.217:1235", "reason": "timeout"},
                ),
            ],
        )

    def test_numeric_ipv4_rejects_hostname(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            diagnostic.numeric_ipv4("vita.local")
        self.assertEqual(
            diagnostic.numeric_ipv4("10.1.1.217"), "10.1.1.217"
        )

    def test_snapshot_parser_accepts_exact_abi(self) -> None:
        values = (
            [diagnostic.VERSION, diagnostic.SNAPSHOT_SIZE, 2,
             diagnostic.EVENT_COUNT]
            + [0] * (diagnostic.EVENT_COUNT * 8)
            + [
                -1, -1, 7, 3, 0, 0,
                diagnostic.STATE_TEST_TITLE_READY, 0, 4, 0,
            ]
        )
        data = struct.pack(diagnostic.SNAPSHOT_FORMAT, *values)
        parsed = diagnostic.parse_snapshot(data)
        self.assertEqual(parsed["size"], 600)
        self.assertEqual(parsed["current"]["listener"], 7)
        self.assertEqual(parsed["current"]["owner_epoch"], 0)
        self.assertTrue(parsed["current"]["test_title_ready"])
        self.assertEqual(parsed["current"]["dropped_event_writes"], 0)

    def test_snapshot_parser_rejects_malformed_size(self) -> None:
        with self.assertRaisesRegex(
            diagnostic.AdmissionDiagnosticFailure,
            "response size",
        ):
            diagnostic.parse_snapshot(b"\0" * (diagnostic.SNAPSHOT_SIZE - 1))


if __name__ == "__main__":
    unittest.main()
