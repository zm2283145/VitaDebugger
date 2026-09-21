import struct
import unittest

from tools.decode_pmu_cleanup_record import (
    COMPLETE_FLAGS,
    BASELINE_OFFSET,
    FINAL_OFFSET,
    FLAG_OWNER_ARMED,
    HANDLES_OFFSET,
    HANDLE_SIZE,
    MAGIC,
    NOT_RUN,
    REARM_ELAPSED_OFFSET,
    REARM_DEADLINE_US,
    RESERVED_OFFSET,
    RESTORED_OFFSET,
    RESULT_AUX_ACTION,
    RESULT_AUX_CLEANUP,
    RESULT_AUX_CREATE,
    RESULT_AUX_DELETE,
    RESULT_AUX_START,
    RESULT_AUX_WAIT,
    RESULT_CLOSE,
    RESULT_NET_CLOSE,
    RESULT_NET_CONNECT,
    RESULT_NET_FAILURE,
    RESULT_NET_PRELUDE,
    RESULT_NET_START,
    RESULT_OPEN,
    RESULT_POST_DISCONNECT_READ,
    RESULT_READ,
    RESULT_REARM_CLOSE,
    RESULT_REARM_OPEN,
    RESULT_REARM_READ,
    RESULT_NET_STOP,
    RESULTS_OFFSET,
    SAMPLE_PADDING_OFFSET,
    SAMPLES_OFFSET,
    SAMPLE_SIZE,
    SIZE,
    STATE_ARMED,
    VERSION,
    VITA_SYSCALL_VP_ERROR_BUSY,
    VP_ERROR_BUSY,
    _fnv1a,
    decode_record,
)
from tools.pmu_cleanup_journal_paths import (
    historical_stage1_filename,
    journal_filename,
)

CAPTURED_STAGE1_SAMPLE = bytes.fromhex(
    "300000000100000001000000010000000100000000000000"
    "050000000000000031000000000000000000000000000000"
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


def put_sample(
    data: bytearray,
    index: int,
    owner_token: int = 7,
    generation: int = 9,
    event_code: int = 0x10,
) -> None:
    struct.pack_into(
        "<8IQ2I",
        data,
        SAMPLES_OFFSET + index * SAMPLE_SIZE,
        48,
        1,
        owner_token,
        generation,
        event_code,
        0,
        5,
        0,
        123,
        0,
        0,
    )


def complete_record(
    stage: int = 1,
    busy_result: int = VP_ERROR_BUSY,
) -> bytearray:
    data = bytearray(SIZE)
    struct.pack_into(
        "<8I2Q",
        data,
        0,
        MAGIC,
        VERSION,
        SIZE,
        0,
        3 if stage >= 4 else 2,
        3,
        stage,
        COMPLETE_FLAGS,
        100,
        200,
    )
    struct.pack_into("<iIiii", data, 48, 0, 0x6B, 0, 0, 0)
    results = [NOT_RUN] * 24
    results[RESULT_REARM_OPEN] = 0
    results[RESULT_REARM_READ] = 0
    results[RESULT_REARM_CLOSE] = 0
    if stage == 1:
        results[RESULT_OPEN] = 0
        results[RESULT_READ] = 0
        results[RESULT_CLOSE] = 0
        results[RESULT_AUX_CREATE] = 1
        results[RESULT_AUX_START] = 0
        results[RESULT_AUX_WAIT] = 0
        results[RESULT_AUX_DELETE] = 0
        results[RESULT_AUX_ACTION] = busy_result
        results[RESULT_AUX_CLEANUP] = NOT_RUN
        struct.pack_into("<II", data, HANDLES_OFFSET + 8, 1, 1)
        data[
            SAMPLES_OFFSET:SAMPLES_OFFSET + SAMPLE_SIZE
        ] = CAPTURED_STAGE1_SAMPLE
    struct.pack_into("<24i", data, RESULTS_OFFSET, *results)
    struct.pack_into(
        "<II", data, HANDLES_OFFSET + HANDLE_SIZE + 8, 2, 2
    )
    put_sample(data, 1, 2, 2, 0x01)
    struct.pack_into("<Q", data, REARM_ELAPSED_OFFSET, 1000)
    put_status(data, BASELINE_OFFSET, 2)
    put_status(data, RESTORED_OFFSET, 2)
    put_status(data, FINAL_OFFSET, 3)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return data


def armed_record() -> bytearray:
    data = complete_record()
    struct.pack_into("<II", data, 16, 2, STATE_ARMED)
    struct.pack_into("<I", data, 24, 5)
    struct.pack_into("<I", data, 28, FLAG_OWNER_ARMED)
    struct.pack_into("<Q", data, 40, 0)
    struct.pack_into("<iIiii", data, 48, 0, 0x6B, 0, NOT_RUN, NOT_RUN)
    struct.pack_into(
        "<24i", data, RESULTS_OFFSET, 0, 0, *([NOT_RUN] * 22)
    )
    struct.pack_into("<II", data, HANDLES_OFFSET + 8, 1, 2)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return data


def stage_three_record() -> bytearray:
    data = complete_record(stage=3)
    for index, result in (
        (RESULT_OPEN, 0),
        (RESULT_READ, 0),
        (RESULT_CLOSE, 0),
        (RESULT_NET_START, 0),
        (RESULT_NET_CONNECT, 0),
        (RESULT_NET_PRELUDE, 0),
        (RESULT_NET_FAILURE, -13),
        (RESULT_NET_CLOSE, 0),
        (RESULT_NET_STOP, 0),
        (RESULT_POST_DISCONNECT_READ, 0),
    ):
        struct.pack_into("<i", data, RESULTS_OFFSET + index * 4, result)
    struct.pack_into("<II", data, HANDLES_OFFSET + 8, 7, 9)
    put_sample(data, 0)
    put_sample(data, 2)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return data


class CleanupRecordTests(unittest.TestCase):
    def test_layout_constants_match_c_abi(self) -> None:
        self.assertEqual(RESULTS_OFFSET, 68)
        self.assertEqual(HANDLES_OFFSET, 164)
        self.assertEqual(HANDLE_SIZE, 40)
        self.assertEqual(SAMPLE_PADDING_OFFSET, 284)
        self.assertEqual(SAMPLES_OFFSET, 288)
        self.assertEqual(SAMPLE_SIZE, 48)
        self.assertEqual(REARM_ELAPSED_OFFSET, 432)
        self.assertEqual(BASELINE_OFFSET, 440)
        self.assertEqual(RESTORED_OFFSET, 624)
        self.assertEqual(FINAL_OFFSET, 808)
        self.assertEqual(RESERVED_OFFSET, 992)

    def test_complete_record_decodes(self) -> None:
        decoded = decode_record(bytes(complete_record()))
        self.assertTrue(decoded["valid"])
        self.assertEqual(decoded["title_id"], "VDCP00013")

    def test_stage_one_retry_provenance_rejects_historical_slots(
        self,
    ) -> None:
        current = decode_record(
            bytes(complete_record()),
            source_journal_name=journal_filename(1, "b"),
            expected_stage=1,
            expected_slot="b",
        )
        self.assertTrue(current["valid"])
        self.assertEqual(
            current["journal_provenance"]["expected_journal_name"],
            "pmu-cleanup-v2-conflict-r2-b.bin",
        )

        historical = decode_record(
            bytes(complete_record()),
            source_journal_name=historical_stage1_filename("b"),
            expected_stage=1,
            expected_slot="b",
        )
        self.assertFalse(historical["valid"])
        self.assertIn(
            "journal_provenance",
            historical["validation_errors"],
        )

    def test_journal_provenance_requires_exact_stage_slot_state(
        self,
    ) -> None:
        wrong_stage = decode_record(
            bytes(complete_record()),
            source_journal_name=journal_filename(2, "b"),
            expected_stage=2,
            expected_slot="b",
        )
        self.assertIn(
            "journal_provenance",
            wrong_stage["validation_errors"],
        )

        wrong_slot = decode_record(
            bytes(complete_record()),
            source_journal_name=journal_filename(1, "a"),
            expected_stage=1,
            expected_slot="a",
        )
        self.assertIn(
            "journal_provenance",
            wrong_slot["validation_errors"],
        )

        with self.assertRaises(ValueError):
            decode_record(
                bytes(complete_record()),
                source_journal_name=journal_filename(1, "b"),
            )

    def test_stage_one_accepts_only_raw_or_exact_syscall_busy(self) -> None:
        for accepted in (VP_ERROR_BUSY, VITA_SYSCALL_VP_ERROR_BUSY):
            with self.subTest(accepted=accepted):
                self.assertTrue(
                    decode_record(
                        bytes(complete_record(busy_result=accepted))
                    )["valid"]
                )
        for rejected in (
            -41,
            -43,
            VITA_SYSCALL_VP_ERROR_BUSY - 1,
            VITA_SYSCALL_VP_ERROR_BUSY + 1,
            -2147352568,
        ):
            with self.subTest(rejected=rejected):
                decoded = decode_record(
                    bytes(complete_record(busy_result=rejected))
                )
                self.assertFalse(decoded["valid"])
                self.assertIn(
                    "completion_contract",
                    decoded["validation_errors"],
                )

    def test_complete_record_requires_bounded_authenticated_rearm(
        self,
    ) -> None:
        for result_index in (
            RESULT_REARM_OPEN,
            RESULT_REARM_READ,
            RESULT_REARM_CLOSE,
        ):
            with self.subTest(result_index=result_index):
                data = complete_record()
                struct.pack_into(
                    "<i",
                    data,
                    RESULTS_OFFSET + result_index * 4,
                    NOT_RUN,
                )
                struct.pack_into("<I", data, 12, _fnv1a(data))
                self.assertIn(
                    "completion_contract",
                    decode_record(bytes(data))["validation_errors"],
                )

        data = complete_record()
        struct.pack_into(
            "<I",
            data,
            SAMPLES_OFFSET + SAMPLE_SIZE + 24,
            4,
        )
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )

        data = complete_record()
        struct.pack_into(
            "<Q",
            data,
            REARM_ELAPSED_OFFSET,
            REARM_DEADLINE_US + 1,
        )
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )

    def test_captured_stage_one_sample_uses_aligned_offset(self) -> None:
        data = complete_record(
            busy_result=VITA_SYSCALL_VP_ERROR_BUSY
        )
        self.assertEqual(
            data[SAMPLES_OFFSET:SAMPLES_OFFSET + SAMPLE_SIZE],
            CAPTURED_STAGE1_SAMPLE,
        )
        decoded = decode_record(bytes(data))
        self.assertTrue(decoded["valid"])
        self.assertEqual(
            decoded["samples"][0],
            {
                "struct_size": 48,
                "abi_version": 1,
                "owner_token": 1,
                "generation": 1,
                "event_code": 1,
                "core_id": 0,
                "physical_counter": 5,
                "flags": 0,
                "value": 49,
            },
        )

    def test_every_record_section_decodes_from_the_c_offsets(self) -> None:
        data = complete_record(
            busy_result=VITA_SYSCALL_VP_ERROR_BUSY
        )
        struct.pack_into(
            "<10I",
            data,
            HANDLES_OFFSET + 2 * HANDLE_SIZE,
            40,
            1,
            0x11223344,
            0x55667788,
            0x10,
            5000,
            0x99AABBCC,
            0xDDEEFF00,
            0,
            0,
        )
        put_sample(
            data,
            2,
            owner_token=0x11223344,
            generation=0x55667788,
            event_code=0x10,
        )
        struct.pack_into(
            "<Q",
            data,
            SAMPLES_OFFSET + 2 * SAMPLE_SIZE + 32,
            0x0123456789ABCDEF,
        )
        struct.pack_into("<I", data, 12, _fnv1a(data))

        decoded = decode_record(bytes(data))
        self.assertEqual(decoded["magic"], MAGIC)
        self.assertEqual(decoded["version"], VERSION)
        self.assertEqual(decoded["size"], SIZE)
        self.assertEqual(
            decoded["results"][RESULT_AUX_ACTION],
            VITA_SYSCALL_VP_ERROR_BUSY,
        )
        self.assertEqual(
            decoded["handles"][2],
            {
                "struct_size": 40,
                "abi_version": 1,
                "owner_token": 0x11223344,
                "generation": 0x55667788,
                "event_code": 0x10,
                "lease_ms": 5000,
                "lease_token_low": 0x99AABBCC,
                "lease_token_high": 0xDDEEFF00,
            },
        )
        self.assertEqual(
            decoded["samples"][2],
            {
                "struct_size": 48,
                "abi_version": 1,
                "owner_token": 0x11223344,
                "generation": 0x55667788,
                "event_code": 0x10,
                "core_id": 0,
                "physical_counter": 5,
                "flags": 0,
                "value": 0x0123456789ABCDEF,
            },
        )
        self.assertEqual(decoded["rearm_elapsed_us"], 1000)
        self.assertEqual(decoded["baseline"]["rearm_count"], 2)
        self.assertEqual(decoded["restored"]["rearm_count"], 2)
        self.assertEqual(decoded["final"]["rearm_count"], 3)
        self.assertEqual(
            decoded["final"]["snapshot"]["raw_pmxevcntr"],
            [14, 15, 16, 17, 18, 19],
        )

    def test_legacy_unaligned_sample_is_rejected(self) -> None:
        data = stage_three_record()
        sample = bytes(
            data[SAMPLES_OFFSET:SAMPLES_OFFSET + SAMPLE_SIZE]
        )
        data[SAMPLES_OFFSET:SAMPLES_OFFSET + SAMPLE_SIZE] = (
            b"\0" * SAMPLE_SIZE
        )
        data[
            SAMPLE_PADDING_OFFSET:
            SAMPLE_PADDING_OFFSET + SAMPLE_SIZE
        ] = sample
        struct.pack_into("<I", data, 12, _fnv1a(data))
        decoded = decode_record(bytes(data))
        self.assertFalse(decoded["valid"])
        self.assertIn("layout_padding", decoded["validation_errors"])
        self.assertIn(
            "completion_contract", decoded["validation_errors"]
        )

    def test_checksum_corruption_fails(self) -> None:
        data = complete_record()
        data[300] ^= 1
        decoded = decode_record(bytes(data))
        self.assertFalse(decoded["valid"])
        self.assertIn("checksum", decoded["validation_errors"])

    def test_snapshot_mismatch_fails(self) -> None:
        data = complete_record()
        struct.pack_into("<I", data, FINAL_OFFSET + 88, 0xDEADBEEF)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        decoded = decode_record(bytes(data))
        self.assertFalse(decoded["valid"])
        self.assertIn("completion_contract", decoded["validation_errors"])

    def test_stage_five_requires_kill_counter_not_normal_exit(self) -> None:
        data = complete_record()
        struct.pack_into("<III", data, 16, 3, 3, 5)
        put_status(data, BASELINE_OFFSET, 2)
        put_status(data, RESTORED_OFFSET, 2, normal_exit_count=1)
        put_status(data, FINAL_OFFSET, 3, normal_exit_count=1)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )
        put_status(data, RESTORED_OFFSET, 2, kill_count=1)
        put_status(data, FINAL_OFFSET, 3, kill_count=1)
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertTrue(decode_record(bytes(data))["valid"])

    def test_stage_three_requires_preserved_post_disconnect_sample(
        self,
    ) -> None:
        data = stage_three_record()
        self.assertTrue(decode_record(bytes(data))["valid"])
        struct.pack_into(
            "<I", data, SAMPLES_OFFSET + 2 * SAMPLE_SIZE + 12, 10
        )
        struct.pack_into("<I", data, 12, _fnv1a(data))
        self.assertIn(
            "completion_contract",
            decode_record(bytes(data))["validation_errors"],
        )
        for offset in (HANDLES_OFFSET + 8, HANDLES_OFFSET + 12):
            data = stage_three_record()
            struct.pack_into("<I", data, offset, 0)
            struct.pack_into("<I", data, 12, _fnv1a(data))
            self.assertIn(
                "completion_contract",
                decode_record(bytes(data))["validation_errors"],
            )

        for bad_error in (NOT_RUN, 0, -12):
            data = stage_three_record()
            struct.pack_into(
                "<i",
                data,
                RESULTS_OFFSET + RESULT_NET_FAILURE * 4,
                bad_error,
            )
            struct.pack_into("<I", data, 12, _fnv1a(data))
            self.assertIn(
                "completion_contract",
                decode_record(bytes(data))["validation_errors"],
            )

        for failed_result in (
            RESULT_CLOSE,
            RESULT_NET_CLOSE,
            RESULT_NET_STOP,
        ):
            data = stage_three_record()
            struct.pack_into(
                "<i", data, RESULTS_OFFSET + failed_result * 4, -1
            )
            struct.pack_into("<I", data, 12, _fnv1a(data))
            self.assertIn(
                "completion_contract",
                decode_record(bytes(data))["validation_errors"],
            )

    def test_stage_three_requires_each_startup_result(self) -> None:
        for index in (
            RESULT_OPEN,
            RESULT_READ,
            RESULT_NET_START,
            RESULT_NET_CONNECT,
            RESULT_NET_PRELUDE,
        ):
            for invalid_result in (NOT_RUN, -1):
                with self.subTest(
                    index=index, invalid_result=invalid_result
                ):
                    data = stage_three_record()
                    struct.pack_into(
                        "<i",
                        data,
                        RESULTS_OFFSET + index * 4,
                        invalid_result,
                    )
                    struct.pack_into("<I", data, 12, _fnv1a(data))
                    self.assertIn(
                        "completion_contract",
                        decode_record(bytes(data))["validation_errors"],
                    )

    def test_stage_three_authenticates_initial_sample(self) -> None:
        sample_offset = SAMPLES_OFFSET
        mismatches = (
            ("struct_size", 0, 44),
            ("abi_version", 4, 2),
            ("owner_token", 8, 8),
            ("generation", 12, 10),
            ("event_code", 16, 0x03),
            ("core_id", 20, 1),
            ("physical_counter", 24, 4),
        )
        for field, offset, value in mismatches:
            with self.subTest(field=field):
                data = stage_three_record()
                struct.pack_into("<I", data, sample_offset + offset, value)
                struct.pack_into("<I", data, 12, _fnv1a(data))
                self.assertIn(
                    "completion_contract",
                    decode_record(bytes(data))["validation_errors"],
                )

    def test_armed_record_requires_nonzero_handle_identity(self) -> None:
        self.assertTrue(decode_record(bytes(armed_record()))["valid"])
        for offset in (HANDLES_OFFSET + 8, HANDLES_OFFSET + 12):
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
