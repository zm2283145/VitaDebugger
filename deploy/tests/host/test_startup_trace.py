from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.startup import (
    STARTUP_IO_MAGIC,
    STARTUP_MAGIC,
    StartupTraceError,
    format_startup_diagnostics,
    parse_startup_io_trace,
    parse_startup_record,
)


class StartupTraceTests(unittest.TestCase):
    def test_existing_v1_startup_record_remains_compatible(self) -> None:
        data = struct.pack("<IIIi", STARTUP_MAGIC, 1, 7, -2147418090)
        record = parse_startup_record(data + b"future-extension")
        self.assertEqual((record.version, record.stage, record.code),
                         (1, 7, -2147418090))

    def test_io_trace_decodes_each_substep_and_signed_error(self) -> None:
        header = struct.pack("<IIII", STARTUP_IO_MAGIC, 1, 16, 16)
        events = b"".join([
            struct.pack("<IIIi", 1, 1, 2, 0),
            struct.pack("<IIIi", 2, 1, 3, 0),
            struct.pack("<IIIi", 3, 8, 2, 0),
            struct.pack("<IIIi", 4, 8, 3, -2147418099),
            struct.pack("<IIIi", 5, 9, 2, 0),
            struct.pack("<IIIi", 6, 9, 3, 0),
            struct.pack("<IIIi", 7, 10, 2, 0),
            struct.pack("<IIIi", 8, 10, 3, 0),
        ])
        trace = parse_startup_io_trace(header + events)
        self.assertEqual(trace.events[3].step_name, "parent_sync")
        self.assertEqual(trace.events[3].event_name, "result")
        rendered = format_startup_diagnostics(
            parse_startup_record(struct.pack("<IIIi", STARTUP_MAGIC, 1, 7,
                                             -2147418099)),
            trace,
        )
        self.assertIn("parent_sync code=-2147418099 (0x8001000D)", rendered)
        self.assertIn("parent_dclose code=0 (0x00000000)", rendered)
        self.assertIn("device_sync code=0 (0x00000000)", rendered)

    def test_torn_final_io_event_is_reported_not_misparsed(self) -> None:
        header = struct.pack("<IIII", STARTUP_IO_MAGIC, 1, 16, 16)
        complete = struct.pack("<IIIi", 1, 1, 2, 0)
        trace = parse_startup_io_trace(header + complete + b"\x02\x00")
        self.assertEqual(len(trace.events), 1)
        self.assertEqual(trace.partial_tail, b"\x02\x00")

    def test_non_monotonic_sequence_is_rejected(self) -> None:
        header = struct.pack("<IIII", STARTUP_IO_MAGIC, 1, 16, 16)
        with self.assertRaisesRegex(StartupTraceError, "expected 1"):
            parse_startup_io_trace(header + struct.pack("<IIIi", 2, 1, 2, 0))


if __name__ == "__main__":
    unittest.main()
