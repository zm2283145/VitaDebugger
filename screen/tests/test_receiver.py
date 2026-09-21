from __future__ import annotations

import json
import socket
import tempfile
import threading
import unittest
import zlib
from pathlib import Path
from unittest import mock

from vdscreen.protocol import (
    FRAME_FLAG_SOURCE_OWNED,
    PIXEL_RGBA8888,
    FrameHeader,
    ProtocolError,
    encode_auth,
    encode_frame,
)
from vdscreen.receiver import (
    DEFAULT_LISTENER_PORT,
    MAX_LISTENER_PORT,
    MIN_LISTENER_PORT,
    LatestFrameStore,
    ReceiverLimits,
    listen_once,
    read_latest,
    receive_connection,
    validate_listener_port,
)


TOKEN = bytes(range(1, 33))
SIDE_BY_SIDE_CONFIG = (
    Path(__file__).resolve().parents[1] /
    "config" / "hardware-gate-side-by-side.json"
)
PIXELS = bytes((
    255, 0, 0, 255,
    0, 255, 0, 255,
    0, 0, 255, 255,
    255, 255, 255, 255,
))


def frame(sequence: int, timestamp_us: int = 1000,
          payload: bytes = PIXELS, session_id: int = 1) -> bytes:
    header = FrameHeader(
        sequence=sequence,
        timestamp_us=timestamp_us,
        width=2,
        height=2,
        stride_bytes=8,
        pixel_format=PIXEL_RGBA8888,
        payload_length=len(payload),
        payload_crc32=zlib.crc32(payload),
        flags=FRAME_FLAG_SOURCE_OWNED,
        session_id=session_id,
    )
    return encode_frame(header, payload)


def run_connection(data: bytes, output: Path,
                   limits: ReceiverLimits = ReceiverLimits(),
                   expected_token: bytes = TOKEN):
    receiver, producer = socket.socketpair()
    result: dict[str, object] = {}

    def worker() -> None:
        try:
            result["stats"] = receive_connection(
                receiver,
                token=expected_token,
                store=LatestFrameStore(output),
                limits=limits,
                peer=("127.0.0.1", 12345),
            )
        except Exception as error:
            result["error"] = error

    thread = threading.Thread(target=worker)
    thread.start()
    producer.sendall(data)
    producer.shutdown(socket.SHUT_WR)
    producer.close()
    thread.join(5)
    if thread.is_alive():
        raise AssertionError("receiver did not terminate")
    return result


class ReceiverTests(unittest.TestCase):
    def test_listener_rejects_invalid_setup_before_socket_creation(self) -> None:
        invalid_limits = ReceiverLimits(max_payload_bytes=0)
        cases = (
            (invalid_limits, TOKEN),
            (ReceiverLimits(), bytes(32)),
        )
        with tempfile.TemporaryDirectory() as directory:
            store = LatestFrameStore(Path(directory))
            for limits, token in cases:
                with self.subTest(limits=limits, zero_token=not any(token)), \
                        mock.patch("vdscreen.receiver.socket.socket") as factory:
                    with self.assertRaises(ValueError):
                        listen_once(
                            bind="127.0.0.1",
                            port=DEFAULT_LISTENER_PORT,
                            allow_lan=False,
                            accept_timeout_seconds=0.1,
                            token=token,
                            store=store,
                            limits=limits,
                        )
                    factory.assert_not_called()

    def test_receive_validation_failure_closes_without_reading(self) -> None:
        class FakeConnection:
            def __init__(self) -> None:
                self.recv_calls = 0
                self.timeout_calls = 0
                self.shutdown_calls = 0
                self.close_calls = 0

            def settimeout(self, timeout: float) -> None:
                del timeout
                self.timeout_calls += 1

            def recv_into(self, buffer: memoryview) -> int:
                del buffer
                self.recv_calls += 1
                return 0

            def shutdown(self, how: int) -> None:
                del how
                self.shutdown_calls += 1

            def close(self) -> None:
                self.close_calls += 1

        cases = (
            (ReceiverLimits(max_payload_bytes=0), TOKEN),
            (ReceiverLimits(), bytes(32)),
        )
        with tempfile.TemporaryDirectory() as directory:
            for limits, token in cases:
                connection = FakeConnection()
                with self.subTest(limits=limits, zero_token=not any(token)):
                    with self.assertRaises(ValueError):
                        receive_connection(
                            connection,
                            token=token,
                            store=LatestFrameStore(Path(directory)),
                            limits=limits,
                        )
                    self.assertEqual(connection.recv_calls, 0)
                    self.assertEqual(connection.timeout_calls, 0)
                    self.assertEqual(connection.shutdown_calls, 1)
                    self.assertEqual(connection.close_calls, 1)

    def test_listener_closes_accepted_socket_if_pre_handoff_raises(self) -> None:
        class FakeAccepted:
            def __init__(self) -> None:
                self.close_calls = 0

            def close(self) -> None:
                self.close_calls += 1

        class FakeListener:
            def __init__(self, accepted: FakeAccepted) -> None:
                self.accepted = accepted

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback):
                del exc_type, exc_value, traceback
                raise ValueError("pre-handoff failure")

            def bind(self, address) -> None:
                del address

            def listen(self, backlog: int) -> None:
                del backlog

            def settimeout(self, timeout: float) -> None:
                del timeout

            def accept(self):
                return self.accepted, ("127.0.0.1", 12345)

        accepted = FakeAccepted()
        listener = FakeListener(accepted)
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch("vdscreen.receiver.socket.socket",
                           return_value=listener):
            with self.assertRaisesRegex(ValueError, "pre-handoff"):
                listen_once(
                    bind="127.0.0.1",
                    port=DEFAULT_LISTENER_PORT,
                    allow_lan=False,
                    accept_timeout_seconds=0.1,
                    token=TOKEN,
                    store=LatestFrameStore(Path(directory)),
                )
        self.assertEqual(accepted.close_calls, 1)

    def test_valid_listener_handoff_closes_connection_once(self) -> None:
        class TrackingConnection:
            def __init__(self, wrapped: socket.socket) -> None:
                self.wrapped = wrapped
                self.close_calls = 0

            def settimeout(self, timeout: float) -> None:
                self.wrapped.settimeout(timeout)

            def recv_into(self, buffer: memoryview) -> int:
                return self.wrapped.recv_into(buffer)

            def shutdown(self, how: int) -> None:
                self.wrapped.shutdown(how)

            def close(self) -> None:
                self.close_calls += 1
                self.wrapped.close()

        class FakeListener:
            def __init__(self, accepted: TrackingConnection) -> None:
                self.accepted = accepted

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback):
                del exc_type, exc_value, traceback
                return False

            def bind(self, address) -> None:
                del address

            def listen(self, backlog: int) -> None:
                del backlog

            def settimeout(self, timeout: float) -> None:
                del timeout

            def accept(self):
                return self.accepted, ("127.0.0.1", 12345)

        receiver, producer = socket.socketpair()
        accepted = TrackingConnection(receiver)
        producer.sendall(encode_auth(TOKEN) + frame(1))
        producer.shutdown(socket.SHUT_WR)
        producer.close()
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch("vdscreen.receiver.socket.socket",
                           return_value=FakeListener(accepted)):
            stats = listen_once(
                bind="127.0.0.1",
                port=DEFAULT_LISTENER_PORT,
                allow_lan=False,
                accept_timeout_seconds=0.1,
                token=TOKEN,
                store=LatestFrameStore(Path(directory)),
            )
        self.assertEqual(stats.frames_published, 1)
        self.assertEqual(accepted.close_calls, 1)

    def test_primary_protocol_error_survives_cleanup_failure(self) -> None:
        class FailingCleanupStore(LatestFrameStore):
            def __init__(self, output: Path) -> None:
                super().__init__(output)
                self.cleanup_calls = 0

            def cleanup_temps(self) -> None:
                self.cleanup_calls += 1
                if self.cleanup_calls > 1:
                    raise OSError("cleanup failed")
                super().cleanup_temps()

        receiver, producer = socket.socketpair()
        producer.sendall(b"bad authentication")
        producer.shutdown(socket.SHUT_WR)
        producer.close()
        shutdown_calls = 0
        close_calls = 0
        original_shutdown = receiver.shutdown
        original_close = receiver.close

        class TrackingConnection:
            def settimeout(self, timeout: float) -> None:
                receiver.settimeout(timeout)

            def recv_into(self, buffer: memoryview) -> int:
                return receiver.recv_into(buffer)

            def shutdown(self, how: int) -> None:
                nonlocal shutdown_calls
                shutdown_calls += 1
                original_shutdown(how)

            def close(self) -> None:
                nonlocal close_calls
                close_calls += 1
                original_close()

        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ProtocolError, "truncated record"):
                receive_connection(
                    TrackingConnection(),
                    token=TOKEN,
                    store=FailingCleanupStore(Path(directory)),
                )
        self.assertEqual(shutdown_calls, 1)
        self.assertEqual(close_calls, 1)

    def test_cleanup_failure_surfaces_after_all_cleanup_attempts(self) -> None:
        class FinalCleanupFailureStore(LatestFrameStore):
            def __init__(self, output: Path) -> None:
                super().__init__(output)
                self.cleanup_calls = 0

            def cleanup_temps(self) -> None:
                self.cleanup_calls += 1
                if self.cleanup_calls > 1:
                    raise OSError("cleanup failed")
                super().cleanup_temps()

        receiver, producer = socket.socketpair()
        producer.sendall(encode_auth(TOKEN))
        producer.shutdown(socket.SHUT_WR)
        producer.close()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(OSError, "cleanup failed"):
                receive_connection(
                    receiver,
                    token=TOKEN,
                    store=FinalCleanupFailureStore(Path(directory)),
                )
        self.assertEqual(receiver.fileno(), -1)

    def test_hardware_gate_manifest_has_distinct_identities(self) -> None:
        manifest = json.loads(SIDE_BY_SIDE_CONFIG.read_text())
        self.assertEqual(manifest["schema_version"], 3)
        host = manifest["host"]
        vita = manifest["vita"]
        coexistence = manifest["coexistence"]
        validate_listener_port(host["screen_listener_port"])
        self.assertEqual(host["screen_listener_port"],
                         vita["screen_connect_port"])
        self.assertEqual(host["screen_receiver"], "vdscreen-v1")
        self.assertEqual(vita["control_listener_port"], 18198)
        self.assertNotEqual(vita["control_listener_port"],
                            vita["screen_connect_port"])
        self.assertIs(vita["screen_control_secrets_distinct"], True)
        self.assertEqual(vita["title_id"], "VDSCRN001")
        self.assertEqual(vita["application_module"],
                         "vitadebug_companion_gate")
        self.assertEqual(vita["archive"], "libvitadebug_companion.a")
        self.assertEqual(vita["artifact"], "vitadebug-companion-gate.vpk")
        self.assertEqual(vita["config_format"], "VDCG-v1-128-byte")
        self.assertEqual(vita["control_bind"], "VITA_PRIVATE_IPV4")
        self.assertEqual(vita["screen_host"], "HOST_PRIVATE_IPV4")
        self.assertIsNone(vita["resident_suprx"])
        self.assertIsNone(vita["resident_skprx"])
        self.assertIs(vita["kernel_api"], False)
        self.assertEqual(
            set(coexistence["forbidden_ports"]), {1337, 1338, 1348})
        self.assertNotIn(host["screen_listener_port"],
                         coexistence["forbidden_ports"])
        self.assertNotIn(vita["control_listener_port"],
                         coexistence["forbidden_ports"])
        self.assertIs(coexistence["mutate_fallback_configuration"], False)
        self.assertIs(coexistence["fallback_remains_installed"], True)
        self.assertIs(coexistence["hardware_contact_authorized"], False)

    def test_side_by_side_listener_port_contract(self) -> None:
        for port in (MIN_LISTENER_PORT, DEFAULT_LISTENER_PORT,
                     MAX_LISTENER_PORT):
            validate_listener_port(port)
        for port in (0, 1337, 1338, 1348, 17999, 18194, 18195, 18196, 18198,
                     19000, 65535):
            with self.subTest(port=port), self.assertRaises(ValueError):
                validate_listener_port(port)

    def test_bind_conflict_closes_failed_listener(self) -> None:
        occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        port = None
        try:
            for candidate in range(18200, 18300):
                try:
                    occupied.bind(("127.0.0.1", candidate))
                    port = candidate
                    break
                except OSError:
                    continue
            self.assertIsNotNone(port, "no test port available")
            occupied.listen(1)
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(OSError):
                    listen_once(
                        bind="127.0.0.1",
                        port=port,
                        allow_lan=False,
                        accept_timeout_seconds=0.1,
                        token=TOKEN,
                        store=LatestFrameStore(Path(directory)),
                    )
        finally:
            occupied.close()
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", port))
        finally:
            probe.close()

    def test_complete_frame_publishes_atomic_ppm_and_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            result = run_connection(encode_auth(TOKEN) + frame(7), output)
            self.assertNotIn("error", result)
            stats = result["stats"]
            self.assertEqual(stats.frames_published, 1)
            metadata, image = read_latest(output)
            self.assertEqual(metadata["sequence"], 7)
            self.assertEqual(metadata["pixel_format"], PIXEL_RGBA8888)
            self.assertEqual(metadata["peer"], "127.0.0.1:12345")
            self.assertEqual(
                image,
                b"P6\n2 2\n255\n" +
                bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255)),
            )
            self.assertEqual(list(output.glob("*.tmp")), [])

    def test_duplicate_is_dropped_and_gap_is_counted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            limits = ReceiverLimits(
                max_frames_per_second=60.0, rate_burst_frames=8)
            data = (encode_auth(TOKEN) + frame(2, 1000) +
                    frame(2, 1000) + frame(5, 2000))
            result = run_connection(data, output, limits)
            self.assertNotIn("error", result)
            stats = result["stats"]
            self.assertEqual(stats.frames_received, 3)
            self.assertEqual(stats.frames_published, 2)
            self.assertEqual(stats.duplicates_dropped, 1)
            self.assertEqual(stats.sequence_gaps, 2)
            metadata = json.loads((output / "latest.json").read_text())
            self.assertEqual(metadata["sequence"], 5)

    def test_bad_token_crc_and_truncation_fail_without_partial_publish(self) -> None:
        bad_reserved = bytearray(encode_auth(TOKEN))
        bad_reserved[49] = 1
        cases = [
            encode_auth(bytes(reversed(TOKEN))) + frame(1),
            bytes(bad_reserved) + frame(1),
            encode_auth(TOKEN) + frame(1)[:-1],
            encode_auth(TOKEN) + frame(1)[:-4] + b"\x00\x00\x00\x00",
        ]
        for data in cases:
            with self.subTest(size=len(data)), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                result = run_connection(data, output)
                self.assertIsInstance(result.get("error"), ProtocolError)
                self.assertFalse((output / "latest.json").exists())
                self.assertEqual(list(output.glob("*.tmp")), [])

    def test_zero_configured_and_wire_tokens_fail_closed(self) -> None:
        zero_wire_auth = bytearray(encode_auth(TOKEN))
        zero_wire_auth[8:40] = bytes(32)
        cases = (
            (encode_auth(TOKEN) + frame(1), bytes(32), ValueError),
            (bytes(zero_wire_auth) + frame(1), TOKEN, ProtocolError),
        )
        for data, expected_token, error_type in cases:
            with self.subTest(error_type=error_type.__name__), \
                    tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                result = run_connection(
                    data, output, expected_token=expected_token)
                self.assertIsInstance(result.get("error"), error_type)
                self.assertFalse((output / "latest.json").exists())
                self.assertEqual(list(output.glob("*.tmp")), [])

    def test_rate_limit_is_connection_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            limits = ReceiverLimits(
                max_frames_per_second=1.0, rate_burst_frames=1)
            result = run_connection(
                encode_auth(TOKEN) + frame(1, 1000) + frame(2, 2000),
                output,
                limits,
            )
            self.assertIsInstance(result.get("error"), ProtocolError)
            metadata, _ = read_latest(output)
            self.assertEqual(metadata["sequence"], 1)

    def test_regressed_sequence_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            limits = ReceiverLimits(
                max_frames_per_second=60.0, rate_burst_frames=4)
            result = run_connection(
                encode_auth(TOKEN) + frame(4, 1000) + frame(3, 2000),
                output,
                limits,
            )
            self.assertIsInstance(result.get("error"), ProtocolError)
            metadata, _ = read_latest(output)
            self.assertEqual(metadata["sequence"], 4)

    def test_mismatched_session_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            result = run_connection(
                encode_auth(TOKEN, session_id=9) +
                frame(1, session_id=10),
                output,
            )
            self.assertIsInstance(result.get("error"), ProtocolError)
            self.assertFalse((output / "latest.json").exists())

    def test_latest_reader_rejects_arbitrary_image_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "latest.json").write_text(json.dumps({
                "complete": True,
                "image": "../outside",
                "image_sha256": "00" * 32,
            }))
            with self.assertRaises(ProtocolError):
                read_latest(output)


if __name__ == "__main__":
    unittest.main()
