import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import rsp_network_retail_gate as gate  # noqa: E402


CORE = b"01234567" * 16
UNAVAILABLE = b"xx" * gate.LEGACY_FPA_BYTES
CPSR = b"00000080"
OBSERVED_SHAPE = CORE + UNAVAILABLE + CPSR


class NullTranscript:
    def __init__(self) -> None:
        self.events = []

    def event(self, event, **fields) -> None:
        self.events.append((event, fields))

    def wire(self, *_args, **_kwargs) -> None:
        pass


class WireSocket:
    def __init__(self, data: bytes):
        self.data = data

    def settimeout(self, _timeout: float) -> None:
        pass

    def sendall(self, _data: bytes) -> None:
        pass

    def recv(self, _size: int) -> bytes:
        data, self.data = self.data, b""
        return data

    def fileno(self) -> int:
        return 1


def wire_client(data: bytes) -> gate.Rsp:
    client = object.__new__(gate.Rsp)
    client.timeout = 1.0
    client.buffer = bytearray()
    client.sock = WireSocket(data)
    client.log = NullTranscript()
    client.case = "framing"
    client.closed = False
    client.ack_mode = True
    return client


class RegisterPayloadTests(unittest.TestCase):
    def assert_rejected(self, payload: bytes) -> None:
        with self.assertRaises(gate.GateFailure):
            gate.require_legacy_register_bank_payload(payload, "registers")

    def test_observed_reply_shape_accepts_complete_unavailable_bytes(self) -> None:
        self.assertEqual(len(OBSERVED_SHAPE), 336)
        self.assertEqual(
            sum(value in gate.HEX_BYTES for value in OBSERVED_SHAPE), 136
        )
        self.assertEqual(OBSERVED_SHAPE.count(b"x"), 200)
        gate.require_legacy_register_bank_payload(
            OBSERVED_SHAPE, "observed retail reply"
        )

    def test_all_hex_reply_is_accepted(self) -> None:
        gate.require_legacy_register_bank_payload(
            b"aF" * gate.LEGACY_G_REPLY_BYTES, "all hex"
        )

    def test_every_unavailable_position_accepts_xx(self) -> None:
        payload = (
            b"00" * gate.CORE_REGISTER_BYTES
            + b"xx" * gate.LEGACY_FPA_BYTES
            + b"ff" * gate.CPSR_BYTES
        )
        gate.require_legacy_register_bank_payload(payload, "all unavailable")

    def test_unavailable_region_accepts_mixed_hex_and_xx_bytes(self) -> None:
        unavailable = b"".join(
            b"xx" if index % 2 else b"5A"
            for index in range(gate.LEGACY_FPA_BYTES)
        )
        gate.require_legacy_register_bank_payload(
            CORE + unavailable + CPSR, "mixed unavailable"
        )

    def test_lone_or_mixed_x_nibbles_are_rejected(self) -> None:
        start = gate.CORE_REGISTER_BYTES * 2
        for pair in (b"x0", b"0x", b"xF", b"Fx", b"XX"):
            payload = bytearray(OBSERVED_SHAPE)
            payload[start:start + 2] = pair
            with self.subTest(pair=pair):
                self.assert_rejected(bytes(payload))

    def test_misaligned_x_across_byte_boundary_is_rejected(self) -> None:
        start = gate.CORE_REGISTER_BYTES * 2
        payload = bytearray(OBSERVED_SHAPE)
        payload[start - 1:start + 1] = b"xx"
        self.assert_rejected(bytes(payload))

    def test_xx_is_rejected_in_required_core_and_cpsr_bytes(self) -> None:
        for offset in (0, len(OBSERVED_SHAPE) - 2):
            payload = bytearray(OBSERVED_SHAPE)
            payload[offset:offset + 2] = b"xx"
            with self.subTest(offset=offset):
                self.assert_rejected(bytes(payload))

    def test_invalid_characters_are_rejected(self) -> None:
        payload = bytearray(OBSERVED_SHAPE)
        payload[gate.CORE_REGISTER_BYTES * 2] = ord("z")
        self.assert_rejected(bytes(payload))

    def test_odd_short_and_long_replies_are_rejected(self) -> None:
        for payload in (
            OBSERVED_SHAPE[:-1],
            OBSERVED_SHAPE[:-2],
            OBSERVED_SHAPE + b"0",
            OBSERVED_SHAPE + b"00",
        ):
            with self.subTest(size=len(payload)):
                self.assert_rejected(payload)


class PacketFramingTests(unittest.TestCase):
    def test_valid_packet_still_parses(self) -> None:
        self.assertEqual(wire_client(gate.frame(b"g")).read_packet(), b"g")

    def test_packet_start_is_required(self) -> None:
        with self.assertRaisesRegex(gate.GateFailure, "packet start"):
            wire_client(b"+" + gate.frame(b"g")).read_packet()

    def test_checksum_must_be_hex(self) -> None:
        with self.assertRaisesRegex(gate.GateFailure, "nonhex checksum"):
            wire_client(b"$g#zz").read_packet()

    def test_checksum_must_match(self) -> None:
        with self.assertRaisesRegex(gate.GateFailure, "checksum"):
            wire_client(b"$g#00").read_packet()

    def test_truncated_frame_is_rejected(self) -> None:
        with self.assertRaises(EOFError):
            wire_client(b"$g").read_packet()


class FailureTelemetryTests(unittest.TestCase):
    def tearDown(self) -> None:
        gate.failure_telemetry = None

    def capture(self, response=None, error=None, *, expected=2):
        transcript = NullTranscript()
        capture = gate.FailureTelemetryCapture(
            "10.1.1.217", 1235, 1.0, expected, transcript)
        client = wire_client(b"")
        client.case = "second-qSupported"
        patch = (
            mock.patch.object(gate, "query_snapshot", side_effect=error)
            if error is not None
            else mock.patch.object(
                gate, "query_snapshot", return_value=response)
        )
        with patch:
            capture.capture(client, TimeoutError("qSupported timed out"))
        return capture.result, transcript

    def test_expected_epoch_is_captured_before_socket_cleanup(self) -> None:
        response = {
            "current": {
                "connection_epoch": 2,
                "dropped_event_writes": 0,
            }
        }
        result, transcript = self.capture(response=response)
        self.assertEqual(result["status"], "captured")
        self.assertTrue(result["socket_open_during_capture"])
        self.assertEqual(transcript.events[-1][0], "failure_telemetry")

    def test_stale_epoch_is_distinct(self) -> None:
        result, _ = self.capture(
            response={
                "current": {
                    "connection_epoch": 1,
                    "dropped_event_writes": 0,
                }
            })
        self.assertEqual(result["status"], "stale_epoch")
        self.assertEqual(result["observed_epoch"], 1)

    def test_dropped_event_write_is_distinct(self) -> None:
        result, _ = self.capture(
            response={
                "current": {
                    "connection_epoch": 2,
                    "dropped_event_writes": 1,
                }
            })
        self.assertEqual(result["status"], "dropped_event_writes")

    def test_bounded_udp_miss_is_distinct(self) -> None:
        result, _ = self.capture(
            error=gate.SnapshotUnavailable("bounded miss"))
        self.assertEqual(result["status"], "udp_unavailable")

    def test_malformed_snapshot_is_distinct(self) -> None:
        result, _ = self.capture(
            error=gate.AdmissionDiagnosticFailure("ABI mismatch"))
        self.assertEqual(result["status"], "malformed_snapshot")

    def test_request_timeout_captures_before_caller_cleanup(self) -> None:
        class Capture:
            def __init__(self):
                self.result = None
                self.calls = []

            def capture(self, client, error):
                self.calls.append((client, error))
                self.result = {"status": "captured"}

        client = wire_client(b"")
        client.sock.recv = mock.Mock(side_effect=TimeoutError("timed out"))
        capture = Capture()
        gate.failure_telemetry = capture
        with self.assertRaises(TimeoutError):
            client.request(b"qSupported")
        self.assertEqual(len(capture.calls), 1)
        captured_client = capture.calls[0][0]
        self.assertFalse(captured_client.closed)

    def test_connect_failure_has_bounded_telemetry(self) -> None:
        transcript = NullTranscript()
        capture = gate.FailureTelemetryCapture(
            "10.1.1.217", 1235, 1.0, 2, transcript)
        with mock.patch.object(
            gate,
            "query_snapshot",
            return_value={
                "current": {
                    "connection_epoch": 1,
                    "dropped_event_writes": 0,
                }
            },
        ):
            capture.capture_connect(
                "second-admission-secondary",
                gate.GateFailure("connect deadline"),
            )
        self.assertEqual(capture.result["status"], "stale_epoch")
        self.assertFalse(capture.result["socket_open_during_capture"])


class SecondAdmissionPhaseTests(unittest.TestCase):
    def tearDown(self) -> None:
        gate.failure_telemetry = None

    def test_phase_runs_only_two_detaching_sessions(self) -> None:
        first = FakeClient([])
        second = FakeClient([])
        calls = []

        def stopped(*_args, **_kwargs):
            calls.append(_args[4])
            if len(calls) == 1:
                return first, 0x3FFFC, {
                    "text_segment": 0x81000000,
                    "data_segment": 0x81001000,
                }
            self.assertTrue(first.detached)
            return second, 0x3FFFC, {
                "text_segment": 0x81000000,
                "data_segment": 0x81001000,
            }

        args = SimpleNamespace(
            host="10.1.1.217",
            port=1234,
            diagnostic_port=1235,
            timeout=1.0,
            expected_epoch=2,
        )
        telemetry = {
            "current": {
                "connection_epoch": 2,
                "dropped_event_writes": 0,
            }
        }
        with (
            mock.patch.object(gate, "stopped_session", side_effect=stopped),
            mock.patch.object(
                gate, "query_snapshot", return_value=telemetry),
        ):
            result = gate.run_second_admission(args, NullTranscript())
        self.assertEqual(
            calls,
            ["second-admission-primary", "second-admission-secondary"],
        )
        self.assertTrue(first.detached)
        self.assertTrue(second.detached)
        self.assertEqual(result["cases"], ["second-admission"])


class MatrixSequencingComplete(RuntimeError):
    pass


class FakeClient:
    def __init__(self, expected: list[tuple[bytes, bytes]]):
        self.expected = expected
        self.closed = False
        self.detached = False

    def request(self, command: bytes, **_kwargs):
        expected_command, response = self.expected.pop(0)
        if command != expected_command:
            raise AssertionError(
                f"expected command {expected_command!r}, got {command!r}"
            )
        return response, b""

    def close(self, **_kwargs) -> None:
        self.closed = True

    def detach(self) -> None:
        self.detached = True


class DisconnectGSequenceTests(unittest.TestCase):
    def test_disconnect_g_accepts_unavailable_markers_before_verification(self) -> None:
        read = b"m1000,4"
        clients = iter(
            [
                FakeClient([(read, b"00000000")]),
                FakeClient([(read, b"00000000")]),
                FakeClient([(read, b"00000000")]),
                FakeClient([(b"M1000,4:00000000", b"OK")]),
                FakeClient([(read, b"00000000")]),
                FakeClient([(b"g", OBSERVED_SHAPE)]),
                FakeClient([(b"g", OBSERVED_SHAPE)]),
            ]
        )

        def stopped(*_args, **_kwargs):
            try:
                client = next(clients)
            except StopIteration as exc:
                raise MatrixSequencingComplete from exc
            return client, gate.MAX_RSP_PAYLOAD, {
                "text_segment": 0x81000000,
                "data_segment": 0x81001000,
            }

        args = SimpleNamespace(
            repo=ROOT,
            host="127.0.0.1",
            port=1234,
            command_port=1338,
            timeout=1.0,
            elf=ROOT / "unused.elf",
            nm=ROOT / "unused-nm",
            cycles=50,
            run_seconds=0.01,
            shutdown_interval=10,
            shutdown_pause=3.0,
            launch_delay=3.0,
            title_id="SLRS00001",
        )
        transcript = NullTranscript()
        with (
            mock.patch.object(gate, "stopped_session", side_effect=stopped),
            mock.patch.object(
                gate, "writable_fixture_address",
                return_value=(0x1000, {"runtime_address": "0x00001000"}),
            ),
        ):
            with self.assertRaises(MatrixSequencingComplete):
                gate.run_matrix(args, transcript)

        passed = [
            fields["case"]
            for event, fields in transcript.events
            if event == "case_pass"
        ]
        self.assertEqual(
            passed, ["disconnect-m", "disconnect-M", "disconnect-g"]
        )


if __name__ == "__main__":
    unittest.main()
