#!/usr/bin/env python3

from __future__ import annotations

import json
import struct
import tempfile
import unittest
from pathlib import Path

import decode_record as decoder


def make_complete(
    revision: int = 3,
    firmware_result: int = 0,
    firmware_version: int = 0x03740000,
) -> bytes:
    data = bytearray(decoder.SIZE)
    flags = (
        decoder.FLAG_MODULE_LOOKUP_OK
        | decoder.FLAG_MODULE_INFO_OK
        | decoder.FLAG_MODULE_NAME_OK
        | decoder.FLAG_EXPORTS_BOUNDED
        | decoder.FLAG_ALL_RESOLVED
        | decoder.FLAG_ALL_EXECUTABLE
        | decoder.FLAG_ALL_CAPTURED
        | decoder.FLAG_COMPLETE
    )
    if firmware_result >= 0:
        flags |= decoder.FLAG_FIRMWARE_QUERY_OK
    struct.pack_into(
        "<7IiIiI3i3I",
        data,
        0,
        decoder.MAGIC,
        decoder.VERSION,
        decoder.SIZE,
        0,
        revision,
        1,
        decoder.STATE_COMPLETE,
        0,
        flags,
        firmware_result,
        firmware_version,
        0,
        0,
        0x10023,
        0xF46ED7B2,
        0x81000100,
        0x81000300,
    )
    name = decoder.EXPECTED_MODULE.encode("ascii") + b"\0"
    data[68 : 68 + len(name)] = name
    struct.pack_into("<5I", data, 96, 1, 4, 4, 4, decoder.ANY_LIBRARY)
    struct.pack_into("<4I", data, 128, 0x81000000, 0x4000, 0x3000, 5)
    for index, (kind, nid, _name) in enumerate(decoder.EXPECTED_TARGETS):
        offset = 192 + index * 100
        code_address = 0x81000400 + index * 0x100
        thumb = index & 1
        target_flags = (
            decoder.TARGET_RESOLVED
            | decoder.TARGET_IN_SEGMENT
            | decoder.TARGET_EXECUTABLE
            | decoder.TARGET_WINDOW_BOUNDED
            | decoder.TARGET_CAPTURED
        )
        if thumb:
            target_flags |= decoder.TARGET_THUMB
        struct.pack_into(
            "<2Ii3Ii2I",
            data,
            offset,
            kind,
            nid,
            0,
            target_flags,
            code_address | thumb,
            code_address,
            0,
            code_address - 0x81000000,
            decoder.CODE_BYTES,
        )
        data[offset + 36 : offset + 100] = bytes(
            (index * 64 + byte) & 0xFF for byte in range(64)
        )
    struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
    return bytes(data)


class ResolverDecoderTests(unittest.TestCase):
    def test_spoofable_firmware_is_not_a_gate(self) -> None:
        record = decoder.parse_record_bytes(make_complete())
        self.assertEqual(record.result, 0)
        self.assertEqual(record.firmware_version, 0x03740000)
        normal_report = decoder.parse_record_bytes(
            make_complete(firmware_version=0x03650000)
        )
        self.assertEqual(normal_report.result, 0)

    def test_failed_firmware_metadata_query_is_not_a_gate(self) -> None:
        record = decoder.parse_record_bytes(
            make_complete(firmware_result=-1, firmware_version=0)
        )
        self.assertEqual(record.result, 0)

    def test_select_newest_handles_revision_wrap(self) -> None:
        old = decoder.parse_record_bytes(make_complete(revision=0xFFFFFFFF))
        new = decoder.parse_record_bytes(make_complete(revision=0))
        self.assertIs(decoder.select_newest([old, new]), new)

    def test_rejects_changed_fixed_nid(self) -> None:
        data = bytearray(make_complete())
        struct.pack_into("<I", data, 196, 0xDEADBEEF)
        struct.pack_into("<I", data, 12, 0)
        struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
        with self.assertRaisesRegex(ValueError, "fixed target"):
            decoder.parse_record_bytes(bytes(data))

    def test_rejects_unbounded_claimed_window(self) -> None:
        data = bytearray(make_complete())
        target = 192 + 3 * 100
        struct.pack_into("<I", data, target + 16, 0x81003FE1)
        struct.pack_into("<I", data, target + 20, 0x81003FE0)
        struct.pack_into("<I", data, target + 28, 0x3FE0)
        struct.pack_into("<I", data, 12, 0)
        struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
        with self.assertRaisesRegex(ValueError, "code-window"):
            decoder.parse_record_bytes(bytes(data))

    def test_extracts_only_fixed_windows_and_metadata(self) -> None:
        record = decoder.parse_record_bytes(make_complete())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            decoder.extract(record, output, "retail 3.65; Enso_ex spoof active")
            binaries = sorted(output.glob("*.bin"))
            self.assertEqual(len(binaries), decoder.TARGET_COUNT)
            self.assertTrue(all(path.stat().st_size == 64 for path in binaries))
            metadata = json.loads((output / "metadata.json").read_text("utf-8"))
            self.assertEqual(
                metadata["actual_test_baseline"],
                "retail 3.65; Enso_ex spoof active",
            )
            self.assertEqual(metadata["reported_firmware_raw"], "0x03740000")
            self.assertTrue(metadata["reported_firmware_is_spoofable"])
            self.assertEqual(len(metadata["targets"]), decoder.TARGET_COUNT)


if __name__ == "__main__":
    unittest.main()
