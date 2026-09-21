import argparse
import ftplib
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import rsp_startup_diagnostic as diagnostic  # noqa: E402


def record(
    sequence: int,
    stage: int,
    *,
    result: int = 0,
    stage_flags: int = 0,
    build_flags: int = 3,
    run_id: int = 0x123456789ABCDEF0,
    failed_stage_mask: int | None = None,
    details: tuple[int, int, int] = (0, 0, 0),
    route_address: int = 0xD901010A,
    bound_address: int = 0,
    bound_port: int = 1235,
    endpoint_result: int = 0,
    self_probe_result: int = 0,
) -> bytes:
    if stage >= 10 and not stage_flags & (
        diagnostic.OBS_SELF_PROBE_RECEIVED
        | diagnostic.OBS_SELF_PROBE_FAILED
    ):
        stage_flags |= diagnostic.OBS_SELF_PROBE_RECEIVED
    if failed_stage_mask is None:
        failed_stage_mask = (
            1 << stage if stage_flags & diagnostic.FAILED else 0
        )
    values = [
        diagnostic.MAGIC,
        diagnostic.VERSION,
        diagnostic.RECORD_SIZE,
        sequence,
        stage,
        result & 0xFFFFFFFF,
        stage_flags,
        build_flags,
        0,
        run_id & 0xFFFFFFFF,
        run_id >> 32,
        failed_stage_mask,
        *(value & 0xFFFFFFFF for value in details),
        route_address,
        bound_address,
        bound_port,
        endpoint_result & 0xFFFFFFFF,
        self_probe_result & 0xFFFFFFFF,
        0,
    ]
    raw = struct.pack(diagnostic.RECORD_FORMAT, *values)
    values[8] = diagnostic.checksum_record(raw)
    return struct.pack(diagnostic.RECORD_FORMAT, *values)


class StartupDiagnosticTests(unittest.TestCase):
    def test_parse_selects_newest_valid_slot(self) -> None:
        parsed = diagnostic.parse_journal(record(12, 12) + record(13, 13))
        self.assertEqual(parsed["selected"]["slot"], 1)
        self.assertEqual(parsed["selected"]["stage_name"], "stdio_ready")
        self.assertTrue(parsed["selected"]["admission_diagnostic"])
        self.assertTrue(parsed["selected"]["console_test"])
        self.assertEqual(parsed["selected"]["route_address"], "10.1.1.217")
        self.assertTrue(parsed["selected"]["route_known"])
        self.assertEqual(parsed["selected"]["bound_address"], "0.0.0.0")
        self.assertEqual(parsed["selected"]["bound_port"], 1235)
        self.assertEqual(parsed["selected"]["endpoint_result"], 0)

    def test_sequence_wrap_selects_newest_slot(self) -> None:
        parsed = diagnostic.parse_journal(
            record(0xFFFFFFFE, 12) + record(1, 13)
        )
        self.assertEqual(parsed["selected"]["sequence"], 1)

    def test_rejects_slots_from_different_runs(self) -> None:
        with self.assertRaises(diagnostic.StartupDiagnosticFailure):
            diagnostic.parse_journal(
                record(12, 12, run_id=1) + record(13, 13, run_id=2)
            )

    def test_bad_checksum_falls_back_to_other_slot(self) -> None:
        damaged = bytearray(record(9, 9))
        damaged[20] ^= 1
        parsed = diagnostic.parse_journal(record(8, 8) + bytes(damaged))
        self.assertEqual(parsed["selected"]["sequence"], 8)
        self.assertEqual(parsed["slot_errors"][0]["slot"], 1)

    def test_rejects_fixed_shape_and_abi_errors(self) -> None:
        for raw in (
            b"",
            b"\0" * (diagnostic.JOURNAL_SIZE - 1),
            b"\0" * diagnostic.JOURNAL_SIZE,
        ):
            with self.subTest(length=len(raw)):
                with self.assertRaises(diagnostic.StartupDiagnosticFailure):
                    diagnostic.parse_journal(raw)

        invalid = bytearray(record(1, 1))
        invalid[0] ^= 1
        with self.assertRaises(diagnostic.StartupDiagnosticFailure):
            diagnostic.parse_journal(bytes(invalid) + bytes(invalid))

    def test_rejects_reserved_flags_and_inconsistent_result(self) -> None:
        cases = [
            record(1, 1, stage_flags=256),
            record(1, 1, build_flags=8),
            record(1, 1, result=-5),
            record(
                1, 1, stage_flags=diagnostic.FAILED,
                failed_stage_mask=0,
            ),
            record(1, 1, run_id=0),
        ]
        for raw in cases:
            with self.subTest(raw=raw):
                with self.assertRaises(diagnostic.StartupDiagnosticFailure):
                    diagnostic.parse_record(raw)

    def test_decodes_failed_stage_and_signed_result(self) -> None:
        parsed = diagnostic.parse_record(
            record(
                4,
                4,
                result=-35,
                stage_flags=diagnostic.FAILED,
                build_flags=4,
                details=(-35, 35, 200),
                self_probe_result=-110,
            )
        )
        self.assertTrue(parsed["failed"])
        self.assertEqual(parsed["result"], -35)
        self.assertTrue(parsed["kernel_mode"])
        self.assertEqual(parsed["details_signed"], [-35, 35, 200])
        self.assertEqual(parsed["self_probe_result"], -110)

    def test_decodes_persistent_self_probe_observations(self) -> None:
        flags = (
            diagnostic.OBS_POLL_ENTERED
            | diagnostic.OBS_SELF_PROBE_RECEIVED
        )
        parsed = diagnostic.parse_record(record(20, 20, stage_flags=flags))
        self.assertTrue(parsed["poll_entered"])
        self.assertTrue(parsed["self_probe_received"])
        self.assertFalse(parsed["self_probe_failed"])

    def test_rejects_invalid_endpoint_and_self_probe_fields(self) -> None:
        received = diagnostic.OBS_SELF_PROBE_RECEIVED
        failed = diagnostic.OBS_SELF_PROBE_FAILED
        cases = [
            record(20, 20, bound_port=70000),
            record(20, 20, stage_flags=received | failed),
            record(20, 20, stage_flags=received, self_probe_result=-1),
            record(20, 20, stage_flags=failed, self_probe_result=0),
            record(20, 20, endpoint_result=-9),
        ]
        for raw in cases:
            with self.subTest(raw=raw):
                with self.assertRaises(diagnostic.StartupDiagnosticFailure):
                    diagnostic.parse_record(raw)

        parsed = diagnostic.parse_record(
            record(
                20, 20, bound_address=0, bound_port=0,
                endpoint_result=-9,
            )
        )
        self.assertEqual(parsed["endpoint_result"], -9)

    def test_accepts_zero_result_failure_and_retains_prior_failure(self) -> None:
        current_failure = diagnostic.parse_record(
            record(4, 4, stage_flags=diagnostic.FAILED)
        )
        self.assertTrue(current_failure["failed"])
        self.assertEqual(current_failure["result"], 0)

        sticky_failure = diagnostic.parse_record(
            record(15, 15, failed_stage_mask=1 << 8)
        )
        self.assertFalse(sticky_failure["failed"])
        self.assertTrue(sticky_failure["any_failed"])
        self.assertEqual(sticky_failure["failed_stages"], ["vfp_fixture"])

    def test_accepts_repeated_poll_stages_with_sticky_later_failure(self) -> None:
        flags = (
            diagnostic.OBS_POLL_ENTERED
            | diagnostic.OBS_REQUEST_RECEIVED
            | diagnostic.OBS_RESPONSE_ATTEMPTED
        )
        failed_mask = 1 << 19
        parsed = diagnostic.parse_journal(
            record(
                100, 17, stage_flags=flags,
                failed_stage_mask=failed_mask,
            )
            + record(
                101, 19, stage_flags=flags,
                failed_stage_mask=failed_mask,
            )
        )
        self.assertTrue(parsed["selected"]["any_failed"])
        self.assertFalse(parsed["selected"]["failed"])

    def test_response_stage_satisfies_poll_progress_wait(self) -> None:
        flags = (
            diagnostic.OBS_POLL_ENTERED
            | diagnostic.OBS_REQUEST_RECEIVED
            | diagnostic.OBS_RESPONSE_ATTEMPTED
            | diagnostic.OBS_RESPONSE_SENT
        )
        raw = record(100, 18, stage_flags=flags) + record(
            101, 19, stage_flags=flags
        )
        with mock.patch.object(
            diagnostic, "fetch_journal", return_value=raw
        ):
            parsed, _returned = diagnostic.observe(
                "10.1.1.217", 1337, 0.1, 20
            )
        self.assertEqual(parsed["selected"]["stage"], 19)
        self.assertTrue(parsed["selected"]["response_sent"])

    def test_retries_transient_ftp_misses_until_success(self) -> None:
        raw = record(14, 14) + record(15, 15)
        with mock.patch.object(
            diagnostic,
            "fetch_journal",
            side_effect=[FileNotFoundError(), OSError(), raw],
        ) as fetch:
            parsed, returned = diagnostic.observe(
                "10.1.1.217", 1337, 1.0, 15
            )
        self.assertEqual(fetch.call_count, 3)
        self.assertEqual(returned, raw)
        self.assertEqual(parsed["selected"]["stage"], 15)

    def test_deadline_returns_latest_incomplete_snapshot(self) -> None:
        raw = record(11, 11) + record(12, 12)
        with (
            mock.patch.object(
                diagnostic, "fetch_journal", return_value=raw
            ),
            mock.patch.object(diagnostic.time, "sleep"),
        ):
            parsed, returned = diagnostic.observe(
                "10.1.1.217", 1337, 0.001, 15
            )
        self.assertEqual(returned, raw)
        self.assertTrue(parsed["deadline_expired"])
        self.assertEqual(parsed["selected"]["stage"], 12)

    def test_rejected_run_id_and_missing_build_flags_expire(self) -> None:
        raw = record(15, 15, build_flags=1, run_id=99) * 2
        with (
            mock.patch.object(
                diagnostic, "fetch_journal", return_value=raw
            ),
            mock.patch.object(diagnostic.time, "sleep"),
        ):
            parsed, _returned = diagnostic.observe(
                "10.1.1.217", 1337, 0.001, 15,
                required_build_flags=3, reject_run_id=99,
            )
        self.assertTrue(parsed["deadline_expired"])
        self.assertIn("rejected baseline", parsed["last_error"])

    def test_deadline_preserves_last_malformed_bytes(self) -> None:
        raw = b"broken"
        with (
            mock.patch.object(
                diagnostic, "fetch_journal", return_value=raw
            ),
            mock.patch.object(diagnostic.time, "sleep"),
        ):
            with self.assertRaises(
                diagnostic.StartupDiagnosticFailure
            ) as raised:
                diagnostic.observe("10.1.1.217", 1337, 0.001, 15)
        self.assertEqual(raised.exception.raw, raw)

    def test_fetch_rejects_oversized_journal(self) -> None:
        ftp = mock.MagicMock()
        ftp.__enter__.return_value = ftp
        ftp.retrbinary.side_effect = (
            lambda _command, callback: callback(
                b"x" * (diagnostic.JOURNAL_SIZE + 1)
            )
        )
        with (
            mock.patch.object(diagnostic.ftplib, "FTP", return_value=ftp),
            self.assertRaises(diagnostic.StartupDiagnosticFailure),
        ):
            diagnostic.fetch_journal("10.1.1.217", 1337, 1.0)

    def test_clear_accepts_absent_file_and_verifies_deletion(self) -> None:
        ftp = mock.MagicMock()
        ftp.__enter__.return_value = ftp
        ftp.delete.side_effect = ftplib.error_perm("550 absent")
        ftp.retrbinary.side_effect = ftplib.error_perm("550 absent")
        with mock.patch.object(
            diagnostic.ftplib, "FTP", return_value=ftp
        ):
            diagnostic.clear_journal("10.1.1.217", 1337, 1.0)
        ftp.delete.assert_called_once_with(diagnostic.REMOTE_PATH)
        ftp.retrbinary.assert_called_once()

    def test_clear_fails_when_journal_remains_present(self) -> None:
        ftp = mock.MagicMock()
        ftp.__enter__.return_value = ftp
        with (
            mock.patch.object(diagnostic.ftplib, "FTP", return_value=ftp),
            self.assertRaises(diagnostic.StartupDiagnosticFailure),
        ):
            diagnostic.clear_journal("10.1.1.217", 1337, 1.0)

    def test_numeric_ipv4_rejects_hostnames_and_noncanonical_literals(self) -> None:
        self.assertEqual(diagnostic.numeric_ipv4("10.1.1.217"), "10.1.1.217")
        for value in ("vita.local", "10.1.1.01", "::1"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    diagnostic.numeric_ipv4(value)


if __name__ == "__main__":
    unittest.main()
