from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.errors import ProtocolError
from vitadevdeploy.protocol import Result
from vitadevdeploy.recovery import collect_recovery_snapshot, parse_promotion_state


JOB = "12" * 16
MARKER = (
    "VITADEVDEPLOY-PROMOTE-1\n"
    f"job={JOB}\n"
    "title_id=TEST00001\n"
    f"manifest_sha256={'ab' * 32}\n"
    "path=ux0:/data/vdd_pkg\n"
    "state=reserved_or_later\n"
).encode("ascii")


class FakeRecoveryFtp:
    def __init__(self, *, marker: bytes | None, result: Result | None = None) -> None:
        self.marker = marker
        self.result = result
        self.files: dict[str, bytes] = {}

    def read_optional_bytes(self, path: str, *, max_size: int) -> bytes | None:
        self.assert_bounded(max_size)
        if path.endswith("promote.state"):
            return self.marker
        return self.files.get(path)

    def read_result(self, job: str) -> Result | None:
        if job != JOB:
            raise AssertionError("unexpected job")
        return self.result

    @staticmethod
    def assert_bounded(max_size: int) -> None:
        if not 0 < max_size <= 1024 * 1024:
            raise AssertionError("unbounded recovery read")


class RecoveryTests(unittest.TestCase):
    def test_promotion_marker_is_strict_and_fixed_path(self) -> None:
        parsed = parse_promotion_state(MARKER)
        self.assertEqual(parsed.job, JOB)
        self.assertEqual(parsed.path, "ux0:/data/vdd_pkg")
        with self.assertRaises(ProtocolError):
            parse_promotion_state(MARKER.replace(b"ux0:/data/vdd_pkg", b"ux0:/app/TEST00001"))
        with self.assertRaises(ProtocolError):
            parse_promotion_state(MARKER + b"extra=yes\n")

    def test_absent_marker_is_the_only_automatic_retry_safe_state(self) -> None:
        snapshot = collect_recovery_snapshot(FakeRecoveryFtp(marker=None))  # type: ignore[arg-type]
        self.assertEqual(snapshot.disposition, "clean")
        self.assertTrue(snapshot.safe_to_retry)

    def test_marker_without_result_is_unknown_and_blocks_retry(self) -> None:
        snapshot = collect_recovery_snapshot(FakeRecoveryFtp(marker=MARKER))  # type: ignore[arg-type]
        self.assertEqual(snapshot.disposition, "outcome_unknown")
        self.assertFalse(snapshot.safe_to_retry)
        self.assertFalse(snapshot.shallow_stage_checked)
        self.assertIn("do not retry", snapshot.operator_action)

    def test_success_result_still_blocks_retry_until_stage_is_reconciled(self) -> None:
        result = Result(JOB, "success", "complete", 0, "TEST00001", "installed")
        fake = FakeRecoveryFtp(marker=MARKER, result=result)
        fake.files[f"/ux0:/data/VitaDevDeploy/results/{JOB}.journal"] = b"journal"
        snapshot = collect_recovery_snapshot(fake)  # type: ignore[arg-type]
        self.assertEqual(snapshot.disposition, "success_reported_marker_stale")
        self.assertFalse(snapshot.safe_to_retry)
        self.assertTrue(snapshot.journal_present)

    def test_failure_result_remains_manual_recovery(self) -> None:
        result = Result(JOB, "failed", "promote", -1, "TEST00001", "failed")
        snapshot = collect_recovery_snapshot(  # type: ignore[arg-type]
            FakeRecoveryFtp(marker=MARKER, result=result)
        )
        self.assertEqual(snapshot.disposition, "failure_reported_marker_stale")
        self.assertFalse(snapshot.safe_to_retry)


if __name__ == "__main__":
    unittest.main()
