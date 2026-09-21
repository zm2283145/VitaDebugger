import struct
import unittest

from tools.decode_pmu_cleanup_record import (
    COMPLETE_FLAGS,
    MAGIC,
    NOT_RUN,
    SIZE,
    VERSION,
    _fnv1a,
    decode_record,
)


def put_status(
    data: bytearray,
    offset: int,
    rearm_count: int,
    normal_exit_count: int = 0,
    kill_count: int = 0,
) -> None:
    values = [
        184,
        1,
        0,
        0,
        0xFFFFFFFF,
        0xFFFFFFFF,
        0,
        0,
        0,
        1,
        0,
        0,
        rearm_count,
        normal_exit_count,
        kill_count,
        0,
        0,
        1,
        0,
        0,
        0,
    ]
    struct.pack_into("<21I", data, offset, *values)
    struct.pack_into("<20I", data, offset + 84, 6, *range(1, 20))


def complete_record() -> bytearray:
    data = bytearray(SIZE)
    struct.pack_into(
        "<8I2Q",
        data,
        0,
        MAGIC,
        VERSION,
        SIZE,
        0,
        2,
        3,
        1,
        COMPLETE_FLAGS,
        100,
        200,
    )
    struct.pack_into("<iIiii", data, 48, 0, 0x6B, 0, 0, 0)
    struct.pack_into("<24i", data, 68, *([NOT_RUN] * 24))
    struct.pack_into("<Q", data, 432, 1000)
    put_status(data, 440, 2)
    put_status(data, 624, 2)
    put_status(data, 808, 3)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return data


class CleanupRecordTests(unittest.TestCase):
    def test_complete_record_decodes(self) -> None:
        decoded = decode_record(bytes(complete_record()))
        self.assertTrue(decoded["valid"])
        self.assertEqual(decoded["title_id"], "VDCP00013")

    def test_checksum_corruption_fails(self) -> None:
        data = complete_record()
        data[300] ^= 1
        decoded = decode_record(bytes(data))
        self.assertFalse(decoded["valid"])
        self.assertIn("checksum", decoded["validation_errors"])

    def test_snapshot_mismatch_fails(self) -> None:
        data = complete_record()
        struct.pack_into("<I", data, 808 + 88, 0xDEADBEEF)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        decoded = decode_record(bytes(data))
        self.assertFalse(decoded["valid"])
        self.assertIn("completion_contract", decoded["validation_errors"])

    def test_stage_five_requires_kill_counter_not_normal_exit(self) -> None:
        data = complete_record()
        struct.pack_into("<III", data, 16, 3, 3, 5)
        put_status(data, 440, 2)
        put_status(data, 624, 2, normal_exit_count=1)
        put_status(data, 808, 3, normal_exit_count=1)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )
        put_status(data, 624, 2, kill_count=1)
        put_status(data, 808, 3, kill_count=1)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertTrue(decode_record(bytes(data))["valid"])


if __name__ == "__main__":
    unittest.main()
