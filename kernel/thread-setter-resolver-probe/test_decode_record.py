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
        | decoder.FLAG_MODULE_NAME_OK
        | decoder.FLAG_ALL_RESOLVED
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
        decoder.NOT_ATTEMPTED,
        0x10023,
        0xF46ED7B2,
        0x81000100,
        0x81000300,
    )
    name = decoder.EXPECTED_MODULE.encode("ascii") + b"\0"
    data[68 : 68 + len(name)] = name
    struct.pack_into("<5I", data, 96, 0, 4, 0, 0, decoder.ANY_LIBRARY)
    struct.pack_into("<I", data, 116, decoder.TARGET_COUNT)
    for index, (kind, nid, _name) in enumerate(decoder.EXPECTED_TARGETS):
        offset = 192 + index * 100
        code_address = 0x81000400 + index * 0x100
        thumb = index & 1
        target_flags = decoder.TARGET_RESOLVED
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
            -1,
            0,
            0,
        )
    struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
    return bytes(data)


class ResolverDecoderTests(unittest.TestCase):
    def test_accepts_durable_target_checkpoint(self) -> None:
        data = bytearray(make_complete())
        struct.pack_into("<I", data, 24, decoder.STATE_TARGET_RECORDED)
        struct.pack_into("<i", data, 28, decoder.NOT_RUN)
        struct.pack_into("<I", data, 32, 0)
        struct.pack_into("<I", data, 100, 2)
        struct.pack_into("<I", data, 116, 2)
        for index in range(2, decoder.TARGET_COUNT):
            offset = 192 + index * 100
            struct.pack_into("<i", data, offset + 8, decoder.NOT_ATTEMPTED)
            struct.pack_into("<I", data, offset + 12, 0)
            struct.pack_into("<I", data, offset + 16, 0)
            struct.pack_into("<I", data, offset + 20, 0)
        struct.pack_into("<I", data, 12, 0)
        struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
        record = decoder.parse_record_bytes(bytes(data))
        self.assertEqual(record.completed_target_count, 2)
        self.assertEqual(record.resolved_count, 2)

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

    def test_rejects_code_capture_claim(self) -> None:
        data = bytearray(make_complete())
        target = 192 + 3 * 100
        flags = struct.unpack_from("<I", data, target + 12)[0]
        struct.pack_into("<I", data, target + 12, flags | decoder.TARGET_CAPTURED)
        struct.pack_into("<I", data, target + 32, decoder.CODE_BYTES)
        data[target + 36] = 0xAA
        struct.pack_into("<I", data, 12, 0)
        struct.pack_into("<I", data, 12, decoder.checksum(bytes(data)))
        with self.assertRaisesRegex(ValueError, "forbidden trust or capture"):
            decoder.parse_record_bytes(bytes(data))

    def test_writes_metadata_without_code_windows(self) -> None:
        record = decoder.parse_record_bytes(make_complete())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            decoder.extract(record, output, "retail 3.65; Enso_ex spoof active")
            binaries = sorted(output.glob("*.bin"))
            self.assertEqual(binaries, [])
            metadata = json.loads((output / "metadata.json").read_text("utf-8"))
            self.assertEqual(
                metadata["actual_test_baseline"],
                "retail 3.65; Enso_ex spoof active",
            )
            self.assertEqual(metadata["reported_firmware_raw"], "0x03740000")
            self.assertTrue(metadata["reported_firmware_is_spoofable"])
            self.assertEqual(len(metadata["targets"]), decoder.TARGET_COUNT)
            self.assertIsNone(metadata["fixed_code_fingerprint_sha256"])
            self.assertTrue(
                all(
                    not target["address_was_dereferenced"]
                    for target in metadata["targets"]
                )
            )


if __name__ == "__main__":
    unittest.main()
