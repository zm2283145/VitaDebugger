import ftplib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tests.host.test_decode_pmu_cleanup_record import _fnv1a, put_status
from tools.decode_pmu_cleanup_record import (
    FLAG_OWNER_ARMED,
    MAGIC,
    NOT_RUN,
)
from tools.pmu_kill_gate import ARMED_PATH, TITLE_ID, run_kill_gate


def armed_record() -> bytes:
    data = bytearray(1024)
    struct.pack_into(
        "<8I2Q",
        data,
        0,
        MAGIC,
        1,
        1024,
        0,
        2,
        2,
        5,
        FLAG_OWNER_ARMED,
        100,
        200,
    )
    struct.pack_into("<iIiii", data, 48, 0, 0x6B, 0, NOT_RUN, NOT_RUN)
    struct.pack_into("<24i", data, 68, 0, 0, *([NOT_RUN] * 22))
    put_status(data, 440, 2)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return bytes(data)


class FakeFtp:
    def __init__(self, responses: list[bytes | None]) -> None:
        self.responses = responses
        self.commands: list[str] = []

    def connect(self, host: str, port: int, timeout: float) -> None:
        self.commands.append(f"connect {host} {port}")

    def login(self) -> None:
        self.commands.append("login")

    def voidcmd(self, command: str) -> None:
        self.commands.append(command)

    def retrbinary(self, command: str, callback) -> None:
        self.commands.append(command)
        response = self.responses.pop(0)
        if response is None:
            raise ftplib.error_perm("550 missing")
        callback(response)

    def quit(self) -> None:
        self.commands.append("quit")

    def close(self) -> None:
        self.commands.append("close")


class FakeCompanion:
    calls: list[tuple[str, str, bool]] = []

    def __init__(self, host: str, timeout: float) -> None:
        self.host = host

    def kill(self, title_id: str, *, require_success: bool = False) -> str:
        self.calls.append((self.host, title_id, require_success))
        return "Killed."


class PmuKillGateTests(unittest.TestCase):
    def setUp(self) -> None:
        FakeCompanion.calls.clear()

    def test_archives_new_armed_record_before_exact_title_kill(self) -> None:
        ftp = FakeFtp([None, b"", armed_record()[:400], armed_record()])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "kill.json"
            armed_copy = root / "armed.bin"
            result = run_kill_gate(
                "192.0.2.17",
                evidence,
                armed_copy,
                1.0,
                0.001,
                ftp_factory=lambda: ftp,
                companion_factory=FakeCompanion,
            )
            self.assertEqual(result["state"], "kill_confirmed")
            self.assertEqual(armed_copy.read_bytes(), armed_record())
            self.assertEqual(
                FakeCompanion.calls,
                [("192.0.2.17", TITLE_ID, True)],
            )
            self.assertEqual(
                json.loads(evidence.read_text(encoding="utf-8"))["state"],
                "kill_confirmed",
            )
            self.assertIn(f"RETR {ARMED_PATH}", ftp.commands)

    def test_existing_armed_record_refuses_kill(self) -> None:
        ftp = FakeFtp([armed_record()])
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "kill.json"
            with self.assertRaisesRegex(RuntimeError, "already exists"):
                run_kill_gate(
                    "192.0.2.17",
                    evidence,
                    Path(directory) / "armed.bin",
                    1.0,
                    0.001,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                )
            self.assertFalse(FakeCompanion.calls)
            self.assertEqual(
                json.loads(evidence.read_text(encoding="utf-8"))["state"],
                "failed",
            )

    def test_existing_host_evidence_refuses_before_contact(self) -> None:
        ftp = FakeFtp([])
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "kill.json"
            evidence.write_text("existing", encoding="utf-8")
            with self.assertRaisesRegex(FileExistsError, "must both be new"):
                run_kill_gate(
                    "192.0.2.17",
                    evidence,
                    Path(directory) / "armed.bin",
                    1.0,
                    0.001,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                )
            self.assertFalse(ftp.commands)
            self.assertFalse(FakeCompanion.calls)


if __name__ == "__main__":
    unittest.main()
