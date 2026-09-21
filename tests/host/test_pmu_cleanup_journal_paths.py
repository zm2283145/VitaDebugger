import re
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tests.host.test_decode_pmu_cleanup_record import complete_record
from tools.pmu_cleanup_journal_paths import (
    DEVICE_PATH_CAPACITY,
    SLOTS,
    device_journal_path,
    ftp_journal_path,
    historical_stage1_filename,
    journal_filename,
)

ROOT = Path(__file__).resolve().parents[2]
HEADER = (
    ROOT
    / "kernel"
    / "pmu-profiler-cleanup-gate"
    / "journal_paths.h"
)


class CleanupJournalPathTests(unittest.TestCase):
    def test_paths_are_unique_ascii_and_bounded(self) -> None:
        paths = [
            device_journal_path(stage, slot)
            for stage in range(1, 6)
            for slot in SLOTS
        ]
        self.assertEqual(len(paths), len(set(paths)))
        for path in paths:
            self.assertLess(
                len(path.encode("ascii")),
                DEVICE_PATH_CAPACITY,
            )
            self.assertTrue(path.startswith("ux0:data/VitaDebugger/"))

    def test_stage_one_retry_cannot_match_historical_slots(self) -> None:
        retry = {journal_filename(1, slot) for slot in SLOTS}
        historical = {
            historical_stage1_filename(slot) for slot in SLOTS
        }
        self.assertTrue(retry.isdisjoint(historical))
        self.assertEqual(
            retry,
            {
                "pmu-cleanup-v2-conflict-r2-a.bin",
                "pmu-cleanup-v2-conflict-r2-b.bin",
                "pmu-cleanup-v2-conflict-r2-c.bin",
            },
        )

    def test_later_stage_names_remain_unchanged(self) -> None:
        self.assertEqual(
            journal_filename(2, "a"),
            "pmu-cleanup-v2-timeout-a.bin",
        )
        self.assertEqual(
            journal_filename(3, "b"),
            "pmu-cleanup-v2-disconnect-b.bin",
        )
        self.assertEqual(
            journal_filename(4, "c"),
            "pmu-cleanup-v2-normal-exit-c.bin",
        )
        self.assertEqual(
            journal_filename(5, "b"),
            "pmu-cleanup-v2-abrupt-exit-b.bin",
        )
        self.assertEqual(
            ftp_journal_path(5, "b"),
            "ux0:/data/VitaDebugger/"
            "pmu-cleanup-v2-abrupt-exit-b.bin",
        )

    def test_python_paths_exactly_mirror_c_macros(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        macros = {}
        lines = header.replace("\\\n", "").splitlines()
        for line in lines:
            match = re.fullmatch(
                r'#define (VD_PMU_CLEANUP_STAGE[1-5](?:_RETRY)?_'
                r'[ABC])\s+"([^"]+)"',
                line.strip(),
            )
            if match:
                macros[match.group(1)] = match.group(2)

        expected = {}
        for stage in range(1, 6):
            retry = "_RETRY" if stage == 1 else ""
            for slot in SLOTS:
                expected[
                    f"VD_PMU_CLEANUP_STAGE{stage}{retry}_"
                    f"{slot.upper()}"
                ] = device_journal_path(stage, slot)
        self.assertEqual(macros, expected)

    def test_invalid_stage_and_slot_fail_closed(self) -> None:
        for stage in (0, 6, True, "1"):
            with self.subTest(stage=stage):
                with self.assertRaises(ValueError):
                    journal_filename(stage, "a")  # type: ignore[arg-type]
        for slot in ("A", "d", "", None):
            with self.subTest(slot=slot):
                with self.assertRaises(ValueError):
                    journal_filename(1, slot)  # type: ignore[arg-type]

    def test_decoder_cli_requires_and_checks_bound_provenance(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "record.bin"
            output = Path(directory) / "record.json"
            record.write_bytes(complete_record())
            base = [
                sys.executable,
                str(ROOT / "tools" / "decode_pmu_cleanup_record.py"),
                str(record),
                "--output",
                str(output),
            ]

            unbound = subprocess.run(
                base,
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(unbound.returncode, 2)
            self.assertIn(
                "bound journal provenance is required",
                unbound.stderr,
            )

            current = subprocess.run(
                base
                + [
                    "--source-journal-name",
                    journal_filename(1, "b"),
                    "--expected-stage",
                    "1",
                    "--expected-slot",
                    "b",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(current.returncode, 0, current.stderr)
            decoded = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(decoded["valid"])

            historical = subprocess.run(
                base
                + [
                    "--source-journal-name",
                    historical_stage1_filename("b"),
                    "--expected-stage",
                    "1",
                    "--expected-slot",
                    "b",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(historical.returncode, 1)
            decoded = json.loads(output.read_text(encoding="utf-8"))
            self.assertIn(
                "journal_provenance",
                decoded["validation_errors"],
            )

            historical_unbound = subprocess.run(
                base + ["--allow-unbound-source"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                historical_unbound.returncode,
                0,
                historical_unbound.stderr,
            )


if __name__ == "__main__":
    unittest.main()
