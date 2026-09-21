import struct
import unittest

from tools.decode_pmu_cleanup_record import (
    COMPLETE_FLAGS,
    FLAG_OWNER_ARMED,
    MAGIC,
    NOT_RUN,
    SIZE,
    STATE_ARMED,
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
        2,
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


def armed_record() -> bytearray:
    data = complete_record()
    struct.pack_into("<II", data, 16, 2, STATE_ARMED)
    struct.pack_into("<I", data, 24, 5)
    struct.pack_into("<I", data, 28, FLAG_OWNER_ARMED)
    struct.pack_into("<Q", data, 40, 0)
    struct.pack_into("<iIiii", data, 48, 0, 0x6B, 0, NOT_RUN, NOT_RUN)
    struct.pack_into("<24i", data, 68, 0, 0, *([NOT_RUN] * 22))
    struct.pack_into("<II", data, 164 + 8, 1, 2)
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

    def test_stage_three_requires_preserved_post_disconnect_sample(
        self,
    ) -> None:
        data = complete_record()
        struct.pack_into("<I", data, 24, 3)
        struct.pack_into("<i", data, 68 + 21 * 4, 0)
        struct.pack_into("<II", data, 164 + 8, 7, 9)
        struct.pack_into(
            "<8IQ2I",
            data,
            284 + 2 * 48,
            48,
            1,
            7,
            9,
            0x10,
            0,
            5,
            0,
            123,
            0,
            0,
        )
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertTrue(decode_record(bytes(data))["valid"])
        struct.pack_into("<I", data, 284 + 2 * 48 + 12, 10)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )

    def test_armed_record_requires_nonzero_handle_identity(self) -> None:
        self.assertTrue(decode_record(bytes(armed_record()))["valid"])
        for offset in (164 + 8, 164 + 12):
            data = bytearray(armed_record())
            struct.pack_into("<I", data, offset, 0)
            struct.pack_into("<I", data, 12, _fnv1a(data))
            decoded = decode_record(bytes(data))
            self.assertFalse(decoded["valid"])
            self.assertIn(
                "armed_contract", decoded["validation_errors"]
            )


if __name__ == "__main__":
    unittest.main()
