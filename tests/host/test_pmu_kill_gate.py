import ftplib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tests.host.test_decode_pmu_cleanup_record import _fnv1a, put_status
from tools.decode_pmu_cleanup_record import (
    BASELINE_OFFSET,
    FLAG_OWNER_ARMED,
    HANDLES_OFFSET,
    MAGIC,
    NOT_RUN,
    RESULTS_OFFSET,
    VERSION,
)
from tools.pmu_kill_gate import (
    ARMED_PATH,
    FAILED_PATH,
    TITLE_ID,
    _read_optional_before,
    run_kill_gate,
)


def armed_record() -> bytes:
    data = bytearray(1024)
    struct.pack_into(
        "<8I2Q",
        data,
        0,
        MAGIC,
        VERSION,
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
    struct.pack_into(
        "<24i", data, RESULTS_OFFSET, 0, 0, *([NOT_RUN] * 22)
    )
    struct.pack_into("<II", data, HANDLES_OFFSET + 8, 1, 2)
    put_status(data, BASELINE_OFFSET, 2)
    struct.pack_into("<I", data, 12, _fnv1a(data))
    return bytes(data)


class FakeDataSocket:
    def __init__(self, blocks: list[bytes]) -> None:
        self.blocks = blocks
        self.timeout = 0.0

    def settimeout(self, timeout: float) -> None:
        self.timeout = timeout

    def recv(self, _size: int) -> bytes:
        return self.blocks.pop(0) if self.blocks else b""

    def close(self) -> None:
        pass


class FakeFtp:
    def __init__(
        self,
        responses: list[bytes | None | tuple[bytes, str]],
    ) -> None:
        self.responses = responses
        self.commands: list[str] = []
        self.timeout = 0.0
        self.sock = None
        self.final_error: str | None = None

    def connect(self, host: str, port: int, timeout: float) -> None:
        self.commands.append(f"connect {host} {port}")

    def login(self) -> None:
        self.commands.append("login")

    def voidcmd(self, command: str) -> None:
        self.commands.append(command)

    def transfercmd(self, command: str) -> FakeDataSocket:
        self.commands.append(command)
        response = self.responses.pop(0)
        if response is None:
            raise ftplib.error_perm("550 missing")
        if isinstance(response, tuple):
            data, self.final_error = response
        else:
            data = response
            self.final_error = None
        return FakeDataSocket([data])

    def voidresp(self) -> str:
        self.commands.append("voidresp")
        if self.final_error is not None:
            error = self.final_error
            self.final_error = None
            raise ftplib.error_perm(error)
        return "226 complete"

    def quit(self) -> None:
        self.commands.append("quit")

    def close(self) -> None:
        self.commands.append("close")


class FakeCompanion:
    calls: list[tuple[str, str, bool]] = []

    def __init__(self, host: str, timeout: float) -> None:
        self.host = host

    def kill(
        self,
        title_id: str,
        *,
        require_success: bool = False,
        deadline: float | None = None,
        monotonic=None,
        before_send=None,
    ) -> str:
        if before_send is not None:
            before_send()
        if (
            deadline is not None
            and monotonic is not None
            and monotonic() >= deadline
        ):
            raise TimeoutError("deadline expired before command transmission")
        self.calls.append((self.host, title_id, require_success))
        return "Killed."


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class PmuKillGateTests(unittest.TestCase):
    def setUp(self) -> None:
        FakeCompanion.calls.clear()

    def test_archives_new_armed_record_before_exact_title_kill(self) -> None:
        ftp = FakeFtp(
            [
                None,
                None,
                None,
                b"",
                None,
                armed_record()[:400],
                None,
                armed_record(),
                None,
            ]
        )
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
            self.assertIn(f"RETR {FAILED_PATH}", ftp.commands)

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

    def test_expired_pre_send_deadline_issues_no_kill(self) -> None:
        clock = FakeClock()

        class ExpiringFtp(FakeFtp):
            def transfercmd(self, command: str) -> FakeDataSocket:
                if (
                    command == f"RETR {FAILED_PATH}"
                    and self.commands.count(command) == 2
                ):
                    clock.now += 2.0
                return super().transfercmd(command)

        ftp = ExpiringFtp(
            [None, None, None, armed_record(), None]
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "kill.json"
            with self.assertRaisesRegex(
                TimeoutError, "deadline expired"
            ):
                run_kill_gate(
                    "192.0.2.17",
                    evidence,
                    root / "armed.bin",
                    10.0,
                    0.1,
                    2.0,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                    monotonic=clock.monotonic,
                    sleep=clock.sleep,
                )
            self.assertFalse(FakeCompanion.calls)
            self.assertEqual(
                json.loads(evidence.read_text(encoding="utf-8"))["state"],
                "failed",
            )

    def test_device_timeout_before_kill_refuses_command(self) -> None:
        ftp = FakeFtp(
            [None, None, None, armed_record(), b"failed-record"]
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "kill.json"
            with self.assertRaisesRegex(
                RuntimeError, "active window expired"
            ):
                run_kill_gate(
                    "192.0.2.17",
                    evidence,
                    root / "armed.bin",
                    10.0,
                    0.1,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                )
            self.assertFalse(FakeCompanion.calls)

    def test_slot_c_payload_then_550_refuses_command(self) -> None:
        ftp = FakeFtp(
            [
                None,
                None,
                None,
                armed_record(),
                (b"failed-record", "550 transfer failed"),
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ftplib.error_perm):
                run_kill_gate(
                    "192.0.2.17",
                    root / "kill.json",
                    root / "armed.bin",
                    10.0,
                    0.1,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                )
            self.assertFalse(FakeCompanion.calls)

    def test_ftp_transfer_rechecks_deadline_between_blocks(self) -> None:
        clock = FakeClock()

        class TricklingSocket(FakeDataSocket):
            def recv(self, size: int) -> bytes:
                clock.now += 1.1
                return super().recv(size)

        class TricklingFtp(FakeFtp):
            def transfercmd(self, command: str) -> FakeDataSocket:
                self.commands.append(command)
                return TricklingSocket([b"first", b"second"])

        ftp = TricklingFtp([])
        with self.assertRaisesRegex(
            TimeoutError, "deadline expired"
        ):
            _read_optional_before(
                ftp,
                ARMED_PATH,
                2.0,
                clock.monotonic,
                "trickling transfer",
            )

    def test_max_kill_delay_cannot_exceed_documented_bound(self) -> None:
        ftp = FakeFtp([])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, r"\(0, 2\]"):
                run_kill_gate(
                    "192.0.2.17",
                    root / "kill.json",
                    root / "armed.bin",
                    10.0,
                    0.1,
                    2.01,
                    ftp_factory=lambda: ftp,
                    companion_factory=FakeCompanion,
                )
            self.assertFalse(ftp.commands)
            self.assertFalse(FakeCompanion.calls)


if __name__ == "__main__":
    unittest.main()
