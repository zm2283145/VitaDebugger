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
) -> bytes:
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
        0,
        0,
        0,
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
            record(1, 1, stage_flags=2),
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
            )
        )
        self.assertTrue(parsed["failed"])
        self.assertEqual(parsed["result"], -35)
        self.assertTrue(parsed["kernel_mode"])

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
