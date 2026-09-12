from __future__ import annotations

import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
AGENT_SOURCE = PROJECT_ROOT / "agent" / "src"


class StartupDiagnosticsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (AGENT_SOURCE / "main.c").read_text(encoding="utf-8")
        cls.io = (AGENT_SOURCE / "io.c").read_text(encoding="utf-8")
        cls.io_header = (AGENT_SOURCE / "io.h").read_text(encoding="utf-8")

    def test_challenge_alone_enables_atomic_trace(self) -> None:
        self.assertIn(
            "vdev_write_atomic(VDEV_CHALLENGE, challenge, (size_t)length, 1,\n"
            "                               record_startup_io, NULL)",
            self.main,
        )
        self.assertIn(
            "vdev_write_atomic(VDEV_PROMOTE_STATE, state, (size_t)length, 0,\n"
            "                             NULL, NULL)",
            self.main,
        )
        self.assertIn("VDEV_STARTUP_IO_TRACE", self.main)
        self.assertNotIn("vdev_write_atomic(VDEV_STARTUP_IO_TRACE", self.main)

    def test_trace_has_all_requested_stable_steps(self) -> None:
        expected = {
            "VDEV_ATOMIC_STEP_OPEN = 1",
            "VDEV_ATOMIC_STEP_WRITE = 2",
            "VDEV_ATOMIC_STEP_FILE_SYNC = 3",
            "VDEV_ATOMIC_STEP_CLOSE = 4",
            "VDEV_ATOMIC_STEP_RENAME = 5",
            "VDEV_ATOMIC_STEP_POSTSTAT = 6",
            "VDEV_ATOMIC_STEP_PARENT_DOPEN = 7",
            "VDEV_ATOMIC_STEP_PARENT_SYNC = 8",
            "VDEV_ATOMIC_STEP_PARENT_DCLOSE = 9",
            "VDEV_ATOMIC_STEP_DEVICE_SYNC = 10",
        }
        for identifier in expected:
            self.assertIn(identifier, self.io_header)
        for step in expected:
            name = step.split(" =", 1)[0]
            self.assertGreaterEqual(self.io.count(name), 2)

    def test_enter_and_result_surround_each_vita_io_call(self) -> None:
        operations = (
            "sceIoOpen(part_path",
            "write_all(descriptor",
            "sceIoSyncByFd(descriptor, 0)",
            "sceIoClose(descriptor)",
            "sceIoRename(part_path, final_path)",
            "sceIoGetstat(final_path, &committed)",
            "sceIoDopen(directory)",
            "sceIoDclose(descriptor)",
            "sceIoSync(VDEV_STORAGE_DEVICE, 0)",
        )
        self.assertGreaterEqual(self.io.count("VDEV_ATOMIC_TRACE_ENTER"),
                                len(operations))
        self.assertGreaterEqual(self.io.count("VDEV_ATOMIC_TRACE_RESULT"),
                                len(operations))
        for operation in operations:
            self.assertIn(operation, self.io)

    def test_trace_writer_is_raw_best_effort_and_never_syncs(self) -> None:
        start = self.main.index("static void record_startup_io(")
        end = self.main.index("static const char *message_for_code", start)
        writer = self.main[start:end]
        self.assertIn("SCE_O_APPEND", writer)
        self.assertIn("raw_startup_io_write", writer)
        self.assertNotIn("vdev_write_atomic", writer)
        self.assertNotIn("sceIoSyncByFd", writer)


if __name__ == "__main__":
    unittest.main()
