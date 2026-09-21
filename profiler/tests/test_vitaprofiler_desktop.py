import queue
import dataclasses
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


PROFILER = Path(__file__).resolve().parents[1]
TOOLS = PROFILER / "tools"
sys.path.insert(0, str(TOOLS))

import vitaprofiler_desktop as desktop  # noqa: E402
import vitaprofiler_trace as trace  # noqa: E402
from test_vitaprofiler_trace import make_capture, make_v2_capture  # noqa: E402


class ViewModelTests(unittest.TestCase):
    def setUp(self):
        self.capture = trace.decode_capture(make_capture())
        self.model = desktop.ProfilerViewModel(
            self.capture, "fixture.vptrace", ("192.168.1.42", 49152))

    def test_metadata_loss_and_timing_views(self):
        metadata = dict(self.model.metadata_rows())
        loss = dict(self.model.loss_rows())

        self.assertEqual(metadata["Events"], "6")
        self.assertEqual(metadata["Peer"], "192.168.1.42:49152")
        self.assertEqual(metadata["Threads"], "1")
        self.assertEqual(metadata["Timestamp unit"], "microseconds")
        self.assertIn("VPRF v1", metadata["Session/process ID"])
        self.assertIn("not encoded", metadata["Module ranges"])
        self.assertIn("not encoded", metadata["ARM/Thumb state"])
        self.assertEqual(loss["Capture integrity"],
                         "No structural loss indicators")
        self.assertIn("VPRF v1", loss["Producer ring drops"])
        self.assertIn("transport-loss", loss["TCP/sink loss"])

        self.assertEqual(len(self.model.zones), 1)
        self.assertEqual(self.model.zones[0].name, "update")
        self.assertEqual(self.model.zones[0].duration_us, 250)
        self.assertEqual(len(self.model.frames), 2)
        self.assertEqual(len(self.model.timeline), 3)
        self.assertIsNone(self.model.frames[0].duration_us)
        self.assertAlmostEqual(self.model.frames[1].fps, 63.2911, places=3)
        self.assertEqual(
            {counter.name for counter in self.model.counters},
            {"draw calls", "vita.memory.free_user_bytes"})

    def test_filtering_and_selection_details(self):
        self.assertEqual(len(self.model.filter_zones("UPDATE")), 1)
        self.assertFalse(self.model.filter_zones("audio"))
        self.assertEqual(len(self.model.filter_counters("memory")), 1)
        self.assertEqual(len(self.model.filter_frames(thread_id=7)), 2)
        self.assertFalse(self.model.filter_frames(thread_id=8))
        self.assertEqual(len(self.model.filter_events("zone_end")), 1)
        bounded = self.model.filter_tables("", None, 1)
        self.assertEqual(len(bounded.timeline.rows), 1)
        self.assertTrue(bounded.timeline.truncated)
        self.assertEqual(len(bounded.events.rows), 1)
        self.assertTrue(bounded.events.truncated)
        self.assertEqual(len(bounded.zones.rows), 1)
        self.assertFalse(bounded.zones.truncated)
        with self.assertRaises(ValueError):
            self.model.filter_tables("", None, 0)

        details = dict(self.model.details(self.capture.events[1]))
        self.assertEqual(details["Name"], "draw calls")
        self.assertEqual(details["Type Name"], "counter")

    def test_v2_identity_loss_and_timeline_views(self):
        capture = trace.decode_capture(make_v2_capture())
        model = desktop.ProfilerViewModel(capture, "live.vptrace")
        metadata = dict(model.metadata_rows())
        loss = dict(model.loss_rows())
        self.assertEqual(metadata["Session state"], "Complete")
        self.assertIn("0x0102030405060708",
                      metadata["Session/process ID"])
        self.assertEqual(metadata["Raw timer source"],
                         "sceKernelGetProcessTimeWide")
        self.assertEqual(metadata["Thread generations"], "1")
        self.assertEqual(metadata["Module ranges"], "1")
        self.assertEqual(metadata["ARM/Thumb state"], "ARM, Thumb")
        self.assertIn("1 dropped", loss["Producer ring drops"])
        self.assertIn("2 transport", loss["TCP/sink loss"])
        self.assertTrue(all(row.thread_generation == 2
                            for row in model.timeline))

    def test_latest_generation_wins_at_same_event_boundary(self):
        identities = (
            trace.ThreadIdentity(7, 1, 0x101, 0, 0),
            trace.ThreadIdentity(7, 2, 0x202, 0, 0),
        )
        capture = dataclasses.replace(
            self.capture, thread_identities=identities)
        model = desktop.ProfilerViewModel(capture, "generations.vptrace")
        self.assertTrue(model.frames)
        self.assertTrue(all(frame.thread_generation == 2
                            for frame in model.frames))


class ControllerTests(unittest.TestCase):
    def test_open_and_both_exports_use_trace_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture_path = root / "capture.vptrace"
            decoded_path = root / "capture.json"
            perfetto_path = root / "capture.perfetto.json"
            capture_path.write_bytes(make_capture())

            controller = desktop.ProfilerController()
            loaded = controller.open_capture(capture_path)
            self.assertIsInstance(loaded.view_model,
                                  desktop.ProfilerViewModel)
            controller.export_decoded(loaded, decoded_path)
            controller.export_perfetto(loaded, perfetto_path)

            self.assertIn('"format": "vitaprofiler-decoded-v1"',
                          decoded_path.read_text("utf-8"))
            self.assertIn('"displayTimeUnit": "ms"',
                          perfetto_path.read_text("utf-8"))

    def test_receiver_validates_and_saves_capture(self):
        raw = make_capture()
        listening: queue.Queue[tuple[str, int]] = queue.Queue()
        outcome: queue.Queue[object] = queue.Queue()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "received.vptrace"
            config = desktop.ReceiveConfig(
                output=output, bind="127.0.0.1", port=0,
                source="127.0.0.1", accept_timeout=3.0,
                idle_timeout=3.0, capture_timeout=3.0)

            def receive() -> None:
                try:
                    outcome.put(desktop.ProfilerController().receive_capture(
                        config, on_listening=listening.put))
                except BaseException as error:
                    outcome.put(error)

            thread = threading.Thread(target=receive)
            thread.start()
            with socket.create_connection(listening.get(timeout=3),
                                          timeout=3) as sender:
                sender.sendall(raw)
                sender.shutdown(socket.SHUT_WR)
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            result = outcome.get_nowait()
            if isinstance(result, BaseException):
                raise result
            self.assertEqual(output.read_bytes(), raw)
            self.assertEqual(len(result.capture.events), 6)
            self.assertEqual(result.peer[0], "127.0.0.1")

    def test_receiver_can_cancel_while_waiting_for_sender(self):
        listening: queue.Queue[tuple[str, int]] = queue.Queue()
        outcome: queue.Queue[BaseException | None] = queue.Queue()
        cancelled = threading.Event()

        def receive() -> None:
            try:
                trace.receive_tcp_once(
                    "127.0.0.1", 0, None, 1024, None, 1.0,
                    listening.put, 5.0, cancelled.is_set)
            except BaseException as error:
                outcome.put(error)
            else:
                outcome.put(None)

        thread = threading.Thread(target=receive)
        thread.start()
        listening.get(timeout=3)
        cancelled.set()
        thread.join(timeout=1)
        self.assertFalse(thread.is_alive())
        self.assertIsInstance(outcome.get_nowait(),
                              trace.TraceReceiveCancelled)

    def test_v2_receiver_publishes_incomplete_live_models(self):
        raw = make_v2_capture()
        chunks: list[bytes] = []
        offset = 0
        while offset < len(raw):
            header = trace.STREAM_V2_CHUNK_HEADER.unpack_from(raw, offset)
            chunk_size = trace.STREAM_V2_CHUNK_HEADER_SIZE + header[5]
            chunks.append(raw[offset:offset + chunk_size])
            offset += chunk_size
        events_index = next(
            index for index, chunk in enumerate(chunks)
            if trace.STREAM_V2_CHUNK_HEADER.unpack_from(chunk)[3] ==
            trace.STREAM_V2_CHUNK_EVENTS)
        listening: queue.Queue[tuple[str, int]] = queue.Queue()
        outcome: queue.Queue[object] = queue.Queue()
        updates: list[desktop.LoadedCapture] = []
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "live.vptrace"
            config = desktop.ReceiveConfig(
                output=output, bind="127.0.0.1", port=0,
                source="127.0.0.1", accept_timeout=3.0,
                idle_timeout=3.0, capture_timeout=3.0)

            def receive() -> None:
                try:
                    outcome.put(desktop.ProfilerController().receive_capture(
                        config, on_listening=listening.put,
                        on_live_update=updates.append))
                except BaseException as error:
                    outcome.put(error)

            thread = threading.Thread(target=receive)
            thread.start()
            with socket.create_connection(listening.get(timeout=3),
                                          timeout=3) as sender:
                sender.sendall(b"".join(chunks[:events_index]))
                time.sleep(2.05)
                sender.sendall(chunks[events_index])
                time.sleep(0.1)
                sender.sendall(b"".join(chunks[events_index + 1:]))
                sender.shutdown(socket.SHUT_WR)
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            result = outcome.get_nowait()
            if isinstance(result, BaseException):
                raise result
            self.assertTrue(result.capture.complete)
            self.assertTrue(updates)
            self.assertTrue(any(not item.capture.complete and
                                item.capture.events for item in updates))
            self.assertEqual(
                updates[-1].capture.events,
                result.capture.events[:len(updates[-1].capture.events)])


if __name__ == "__main__":
    unittest.main()
