from __future__ import annotations

import json
import socket
import tempfile
import threading
import unittest
import zlib
from pathlib import Path

from vdscreen.protocol import (
    FRAME_FLAG_SOURCE_OWNED,
    PIXEL_RGBA8888,
    FrameHeader,
    ProtocolError,
    encode_auth,
    encode_frame,
)
from vdscreen.receiver import (
    LatestFrameStore,
    ReceiverLimits,
    read_latest,
    receive_connection,
)


TOKEN = bytes(range(1, 33))
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
                   limits: ReceiverLimits = ReceiverLimits()):
    receiver, producer = socket.socketpair()
    result: dict[str, object] = {}

    def worker() -> None:
        try:
            result["stats"] = receive_connection(
                receiver,
                token=TOKEN,
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
