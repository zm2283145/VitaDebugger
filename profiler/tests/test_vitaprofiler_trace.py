import contextlib
import io
import json
import queue
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


PROFILER = Path(__file__).resolve().parents[1]
TOOLS = PROFILER / "tools"
sys.path.insert(0, str(TOOLS))

import vitaprofiler_trace as trace  # noqa: E402


def encode_dictionary(custom_names: list[str]) -> bytes:
    entries: list[tuple[int, bytes, int]] = [
        (name_id, name.encode("utf-8"), trace.NAME_FLAG_BUILTIN)
        for name_id, name in trace.BUILTIN_NAMES.items()
    ]
    entries.extend((trace.fnv1a_name_id(name.encode("utf-8")),
                    name.encode("utf-8"), 0)
                   for name in custom_names)
    entries.sort(key=lambda item: item[0])
    body = bytearray()
    for name_id, name, flags in entries:
        padded = (len(name) + 3) & ~3
        body.extend(trace.NAME_ENTRY_HEADER.pack(name_id, len(name), flags))
        body.extend(name)
        body.extend(b"\0" * (padded - len(name)))
    total = trace.NAME_HEADER_SIZE + len(body)
    return (trace.NAME_HEADER.pack(
        trace.NAME_MAGIC, trace.NAME_VERSION, trace.NAME_HEADER_SIZE,
        trace.NAME_ENTRY_HEADER_SIZE, trace.NAME_FLAGS, len(entries), total, 0)
        + body)


def encode_event(timestamp: int, value: int, name_id: int, thread_id: int,
                 correlation: int, event_type: int, flags: int = 0) -> bytes:
    return trace.WIRE_EVENT.pack(timestamp, value, name_id, thread_id,
                                 correlation, event_type, flags)


def make_capture() -> bytes:
    names = encode_dictionary(["update", "draw calls", "main frame"])
    update = trace.fnv1a_name_id(b"update")
    draws = trace.fnv1a_name_id(b"draw calls")
    frame = trace.fnv1a_name_id(b"main frame")
    header = trace.WIRE_HEADER.pack(
        trace.WIRE_MAGIC, trace.WIRE_VERSION, trace.WIRE_HEADER_SIZE,
        trace.WIRE_EVENT_SIZE, trace.WIRE_FLAGS, trace.WIRE_CLOCK_HZ, 900, 0)
    events = b"".join([
        encode_event(1000, 0, update, 7, 1, trace.EVENT_ZONE_BEGIN),
        encode_event(1100, 42, draws, 7, 0, trace.EVENT_COUNTER),
        encode_event(1200, 0, frame, 7, 0, trace.EVENT_FRAME,
                     trace.EVENT_FLAG_FIRST),
        encode_event(17000, 15800, frame, 7, 1, trace.EVENT_FRAME),
        encode_event(1250, 250, update, 7, 1, trace.EVENT_ZONE_END),
        encode_event(1300, 1024, 0xFFF00001, 7, 0,
                     trace.EVENT_MEMORY_SAMPLE),
    ])
    return names + header + events


class DecodeTests(unittest.TestCase):
    def test_c_generated_capture_matches_python_decoder(self):
        fixture = PROFILER / "build" / "host" / "capture-from-c.vptrace"
        self.assertTrue(fixture.is_file(),
                        "make host-test must generate the C wire fixture")
        capture = trace.decode_capture(fixture.read_bytes())
        self.assertEqual(capture.header.stream_start_us, 900)
        self.assertEqual(len(capture.events), 6)
        self.assertEqual(capture.resolve_name(capture.events[0].name_id),
                         "update")
        self.assertEqual(capture.events[1].value, 42)
        self.assertEqual(capture.events[5].name_id, 0xFFF00001)

    def test_named_capture_summary_and_exports(self):
        capture = trace.decode_capture(make_capture())
        self.assertEqual(len(capture.events), 6)
        self.assertEqual(capture.resolve_name(capture.events[0].name_id),
                         "update")
        self.assertEqual(capture.resolve_name(0xFFF00001),
                         "vita.memory.free_user_bytes")
        zones = trace.analyze_zones(capture)
        self.assertEqual(len(zones.spans), 1)
        self.assertEqual(zones.spans[0].duration_us, 250)

        summary = trace.render_summary(capture)
        self.assertIn("update", summary)
        self.assertIn("draw calls", summary)
        self.assertIn("63.29 FPS", summary)
        self.assertIn("0 referenced IDs unresolved", summary)

        decoded = trace.capture_to_json(capture)
        self.assertEqual(decoded["events"][1]["name"], "draw calls")
        chrome = trace.capture_to_chrome_trace(capture)
        complete = [event for event in chrome["traceEvents"]
                    if event.get("ph") == "X"]
        self.assertEqual(complete[0]["name"], "update")
        self.assertEqual(complete[0]["dur"], 250)
        self.assertTrue(any(event.get("ph") == "C" and
                            event.get("name") == "draw calls"
                            for event in chrome["traceEvents"]))

    def test_vprf_without_dictionary_keeps_numeric_fallback(self):
        raw = make_capture()
        dictionary_size = struct.unpack_from("<I", raw, 16)[0]
        capture = trace.decode_capture(raw[dictionary_size:])
        self.assertFalse(capture.names)
        self.assertEqual(capture.resolve_name(capture.events[0].name_id),
                         f"name_0x{capture.events[0].name_id:08x}")

    def test_rejects_truncation_and_header_corruption(self):
        raw = make_capture()
        with self.assertRaisesRegex(trace.TraceFormatError, "partial event"):
            trace.decode_capture(raw[:-1])
        damaged = bytearray(raw)
        dictionary_size = struct.unpack_from("<I", damaged, 16)[0]
        damaged[dictionary_size + 24] = 1
        with self.assertRaisesRegex(trace.TraceFormatError, "reserved"):
            trace.decode_capture(bytes(damaged))
        damaged = bytearray(raw)
        damaged[4] = trace.NAME_VERSION + 1
        with self.assertRaisesRegex(trace.TraceFormatError, "unsupported VPNM"):
            trace.decode_capture(bytes(damaged))

    def test_rejects_name_hash_padding_and_builtin_tampering(self):
        raw = bytearray(make_capture())
        _, _, _, _, _, entry_count, dictionary_size, _ = (
            trace.NAME_HEADER.unpack_from(raw))
        offset = trace.NAME_HEADER_SIZE
        user_offset = None
        padded_offset = None
        builtin_offset = None
        for _ in range(entry_count):
            name_id, length, _flags = trace.NAME_ENTRY_HEADER.unpack_from(raw,
                                                                          offset)
            padded = (length + 3) & ~3
            if name_id not in trace.BUILTIN_NAMES and user_offset is None:
                user_offset = offset
            if padded != length and padded_offset is None:
                padded_offset = offset + trace.NAME_ENTRY_HEADER_SIZE + length
            if name_id in trace.BUILTIN_NAMES and builtin_offset is None:
                builtin_offset = offset
            offset += trace.NAME_ENTRY_HEADER_SIZE + padded
        self.assertEqual(offset, dictionary_size)
        self.assertIsNotNone(user_offset)
        self.assertIsNotNone(padded_offset)
        self.assertIsNotNone(builtin_offset)

        damaged = bytearray(raw)
        damaged[user_offset + trace.NAME_ENTRY_HEADER_SIZE] ^= 1
        with self.assertRaisesRegex(trace.TraceFormatError, "failed validation"):
            trace.decode_capture(bytes(damaged))
        damaged = bytearray(raw)
        damaged[padded_offset] = 1
        with self.assertRaisesRegex(trace.TraceFormatError, "nonzero padding"):
            trace.decode_capture(bytes(damaged))
        damaged = bytearray(raw)
        damaged[builtin_offset + 6] = 0
        damaged[builtin_offset + 7] = 0
        with self.assertRaisesRegex(trace.TraceFormatError, "redefined"):
            trace.decode_capture(bytes(damaged))

    def test_unmatched_zones_are_visible_diagnostics(self):
        raw = make_capture()
        dictionary_size = struct.unpack_from("<I", raw, 16)[0]
        partial = raw[:dictionary_size + trace.WIRE_HEADER_SIZE +
                      trace.WIRE_EVENT_SIZE]
        capture = trace.decode_capture(partial)
        analysis = trace.analyze_zones(capture)
        self.assertEqual(len(analysis.unmatched_begins), 1)
        self.assertIn("unmatched zone begins: 1",
                      trace.render_summary(capture))
        chrome = trace.capture_to_chrome_trace(capture)
        self.assertTrue(any(event.get("cat") == "diagnostic.unmatched_zone"
                            for event in chrome["traceEvents"]))

    def test_decode_rejects_unsafe_names_and_event_amplification(self):
        raw = make_capture()
        dictionary_size = struct.unpack_from("<I", raw, 16)[0]
        unsafe = encode_dictionary(["\x1b[31mspoofed"]) + raw[dictionary_size:]
        with self.assertRaisesRegex(trace.TraceFormatError, "display controls"):
            trace.decode_capture(unsafe)

        for separator in ("\u2028", "\u2029"):
            with self.subTest(separator=ord(separator)):
                unsafe = (encode_dictionary([f"spoofed{separator}line"])
                          + raw[dictionary_size:])
                with self.assertRaisesRegex(trace.TraceFormatError,
                                            "display controls"):
                    trace.decode_capture(unsafe)

        header = trace.WIRE_HEADER.pack(
            trace.WIRE_MAGIC, trace.WIRE_VERSION, trace.WIRE_HEADER_SIZE,
            trace.WIRE_EVENT_SIZE, trace.WIRE_FLAGS, trace.WIRE_CLOCK_HZ,
            0, 0)
        oversized = header + b"\0" * (
            (trace.MAX_DECODED_EVENTS + 1) * trace.WIRE_EVENT_SIZE)
        with self.assertRaisesRegex(trace.TraceFormatError, "decoded limit"):
            trace.decode_capture(oversized)

    def test_zone_matching_handles_reused_correlations_by_name(self):
        events = (
            trace.Event(0, 10, 0, 1, 7, 9, trace.EVENT_ZONE_BEGIN, 0),
            trace.Event(1, 11, 0, 2, 7, 9, trace.EVENT_ZONE_BEGIN, 0),
            trace.Event(2, 12, 2, 1, 7, 9, trace.EVENT_ZONE_END, 0),
            trace.Event(3, 13, 3, 2, 7, 9, trace.EVENT_ZONE_END, 0),
        )
        capture = trace.TraceCapture(
            trace.TraceHeader(1, 1, 1_000_000, 0), {}, events, 0,
            trace.WIRE_HEADER_SIZE + len(events) * trace.WIRE_EVENT_SIZE)
        analysis = trace.analyze_zones(capture)
        self.assertEqual([span.name_id for span in analysis.spans], [1, 2])
        self.assertEqual(len(analysis.duplicate_begins), 1)
        self.assertFalse(analysis.unmatched_begins)
        self.assertFalse(analysis.unmatched_ends)


class ReceiverTests(unittest.TestCase):
    def test_fragmented_loopback_tcp_capture(self):
        raw = make_capture()
        listening: queue.Queue[tuple[str, int]] = queue.Queue()
        outcome: queue.Queue[object] = queue.Queue()

        def server() -> None:
            try:
                result = trace.receive_tcp_once(
                    "127.0.0.1", 0, "127.0.0.1", len(raw) + 16,
                    3.0, 3.0, listening.put)
                outcome.put(result)
            except BaseException as error:
                outcome.put(error)

        thread = threading.Thread(target=server)
        thread.start()
        address = listening.get(timeout=3)
        with socket.create_connection(address, timeout=3) as sender:
            for offset in range(0, len(raw), 7):
                sender.sendall(raw[offset:offset + 7])
            sender.shutdown(socket.SHUT_WR)
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        result = outcome.get_nowait()
        if isinstance(result, BaseException):
            raise result
        received, peer = result
        self.assertEqual(received, raw)
        self.assertEqual(peer[0], "127.0.0.1")
        self.assertEqual(len(trace.decode_capture(received).events), 6)

    def test_receiver_enforces_hard_byte_bound(self):
        left, right = socket.socketpair()
        with left, right:
            right.sendall(b"x" * 65)
            right.shutdown(socket.SHUT_WR)
            with self.assertRaisesRegex(trace.TraceReceiveError,
                                        "safety limit"):
                trace.receive_socket(left, 64)

    def test_receiver_enforces_absolute_capture_deadline(self):
        left, right = socket.socketpair()
        with left, right:
            right.sendall(b"x")
            start = time.monotonic()
            with self.assertRaisesRegex(trace.TraceReceiveError,
                                        "total time limit"):
                trace.receive_socket(left, 64, idle_timeout=1.0,
                                     total_timeout=0.05)
            self.assertLess(time.monotonic() - start, 0.5)

    def test_receivers_reject_non_finite_timeouts(self):
        left, right = socket.socketpair()
        with left, right:
            for value in (float("nan"), float("inf"), float("-inf")):
                with self.subTest(kind="idle", value=value):
                    with self.assertRaises(ValueError):
                        trace.receive_socket(left, 64, idle_timeout=value)
                with self.subTest(kind="capture", value=value):
                    with self.assertRaises(ValueError):
                        trace.receive_socket(left, 64, total_timeout=value)
                with self.subTest(kind="accept", value=value):
                    with self.assertRaises(ValueError):
                        trace.receive_tcp_once("127.0.0.1", 0, None, 64,
                                               value, 1.0)


class CommandLineTests(unittest.TestCase):
    def test_view_and_both_json_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture_path = root / "capture.vptrace"
            decoded_path = root / "capture.json"
            chrome_path = root / "capture.perfetto.json"
            capture_path.write_bytes(make_capture())

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(trace.main([
                    "view", str(capture_path), "--events", "2"]), 0)
            self.assertIn("VitaProfiler capture", output.getvalue())
            self.assertIn("more events", output.getvalue())

            self.assertEqual(trace.main([
                "json", str(capture_path), str(decoded_path)]), 0)
            self.assertEqual(trace.main([
                "chrome", str(capture_path), str(chrome_path)]), 0)
            self.assertEqual(json.loads(decoded_path.read_text("utf-8"))
                             ["events"][0]["name"], "update")
            self.assertEqual(json.loads(chrome_path.read_text("utf-8"))
                             ["metadata"]["event_count"], 6)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(trace.main([
                    "json", str(capture_path), str(decoded_path)]), 2)

    def test_no_force_publish_cannot_clobber_racing_creator(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.json"
            original_link = trace.os.link

            def racing_link(source: Path, destination: Path) -> None:
                Path(destination).write_bytes(b"winner")
                original_link(source, destination)

            with mock.patch.object(trace.os, "link", side_effect=racing_link):
                with self.assertRaises(FileExistsError):
                    trace._write_atomic(output, b"loser", force=False)
            self.assertEqual(output.read_bytes(), b"winner")

    def test_cli_rejects_non_finite_timeouts(self):
        parser = trace.build_argument_parser()
        for option in ("--accept-timeout", "--idle-timeout",
                       "--capture-timeout"):
            for value in ("nan", "inf", "-inf"):
                with self.subTest(option=option, value=value):
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            parser.parse_args([
                                "receive", "capture.vptrace", option, value,
                            ])

    def test_local_capture_read_is_bounded_by_bytes_actually_read(self):
        with tempfile.TemporaryDirectory() as directory:
            capture_path = Path(directory) / "growing.vptrace"
            capture_path.write_bytes(b"x" * 65)
            with self.assertRaisesRegex(trace.TraceFormatError,
                                        "64-byte safety limit"):
                trace._read_capture(capture_path, 64)


if __name__ == "__main__":
    unittest.main()
