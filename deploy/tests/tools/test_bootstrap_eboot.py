from __future__ import annotations

import contextlib
import ftplib
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "bootstrap_eboot.py"
SPEC = importlib.util.spec_from_file_location("bootstrap_eboot", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
bootstrap_eboot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bootstrap_eboot
SPEC.loader.exec_module(bootstrap_eboot)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FakeFTP:
    def __init__(self, files: dict[str, bytes] | None = None) -> None:
        self.files = dict(files or {})
        self.size_overrides: dict[str, int] = {}
        self.rename_failures: dict[tuple[str, str], int] = {}
        self.events: list[tuple] = []

    def connect(self, host: str, port: int, timeout: float) -> None:
        self.events.append(("connect", host, port, timeout))

    def login(self) -> None:
        self.events.append(("login",))

    def voidcmd(self, command: str) -> str:
        self.events.append(("voidcmd", command))
        return "200 OK"

    def size(self, path: str) -> int:
        self.events.append(("size", path))
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        return self.size_overrides.get(path, len(self.files[path]))

    def retrbinary(self, command: str, callback, blocksize: int = 8192) -> str:
        operation, path = command.split(" ", 1)
        assert operation == "RETR"
        self.events.append(("retr", path))
        data = self.files[path]
        for offset in range(0, len(data), max(1, blocksize // 2)):
            callback(data[offset : offset + max(1, blocksize // 2)])
        return "226 complete"

    def storbinary(self, command: str, source, blocksize: int = 8192) -> str:
        operation, path = command.split(" ", 1)
        assert operation == "STOR"
        self.events.append(("stor", path))
        chunks = []
        while True:
            chunk = source.read(blocksize)
            if not chunk:
                break
            chunks.append(chunk)
        self.files[path] = b"".join(chunks)
        return "226 complete"

    def rename(self, source: str, target: str) -> str:
        self.events.append(("rename", source, target))
        key = (source, target)
        if self.rename_failures.get(key, 0) > 0:
            self.rename_failures[key] -= 1
            raise ftplib.error_perm("550 injected rename failure")
        if source not in self.files:
            raise ftplib.error_perm("550 missing source")
        self.files[target] = self.files.pop(source)
        return "250 renamed"

    def delete(self, path: str) -> str:
        self.events.append(("delete", path))
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        del self.files[path]
        return "250 deleted"

    def quit(self) -> None:
        self.events.append(("quit",))

    def close(self) -> None:
        self.events.append(("close",))


class Harness:
    def __init__(
        self,
        case: unittest.TestCase,
        original: bytes = b"original eboot contents",
        live: bytes | None = None,
        remote_backup: bytes | None = None,
    ) -> None:
        self.case = case
        self.original = original
        self.temp = tempfile.TemporaryDirectory()
        self.backup_dir = Path(self.temp.name)
        paths = bootstrap_eboot.RemotePaths.for_title("SLRS00001")
        files = {paths.live: original if live is None else live}
        if remote_backup is not None:
            files[paths.backup] = remote_backup
        self.ftp = FakeFTP(files)
        self.commands: list[tuple[str, str, str]] = []

        def command_sender(ip: str, title: str, action: str) -> str:
            self.ftp.events.append(("command", action, title))
            self.commands.append((ip, title, action))
            return {
                "kill": bootstrap_eboot.COMPANION_KILL_OK,
                "launch": bootstrap_eboot.COMPANION_LAUNCH_OK,
            }[action]

        self.manager = bootstrap_eboot.BootstrapEbootManager(
            "10.1.1.93",
            "SLRS00001",
            sha(original),
            backup_dir=self.backup_dir,
            ftp_factory=lambda: self.ftp,
            command_sender=command_sender,
            sleeper=lambda _seconds: None,
        )

    def write_pc_backup(self, data: bytes | None = None) -> Path:
        path = self.manager.pc_backup
        path.write_bytes(self.original if data is None else data)
        return path

    def close(self) -> None:
        self.temp.cleanup()


class BootstrapEbootTests(unittest.TestCase):
    def test_title_is_validated_and_pinned(self) -> None:
        self.assertEqual(bootstrap_eboot.validate_title("SLRS00001"), "SLRS00001")
        for invalid in ("SLRS0000", "slrs00001", "SLRS0000!", "../SLRS01"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(bootstrap_eboot.SafetyError):
                    bootstrap_eboot.validate_title(invalid)
        with self.assertRaises(bootstrap_eboot.SafetyError):
            bootstrap_eboot.validate_title("OTHER0001")

    def test_cli_requires_explicit_connection_title_and_hash(self) -> None:
        parser = bootstrap_eboot.build_parser()
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                parser.parse_args(["status"])
        parsed = parser.parse_args(
            [
                "status",
                "--vita-ip",
                "10.1.1.93",
                "--title",
                "SLRS00001",
                "--expected-sha256",
                "a" * 64,
            ]
        )
        self.assertEqual(parsed.vita_ip, "10.1.1.93")

    def test_companion_commands_use_port_1338_newline_and_title_scope(self) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent = b""
                self.timeout = None

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def settimeout(self, timeout: float) -> None:
                self.timeout = timeout

            def sendall(self, payload: bytes) -> None:
                self.sent += payload

            def shutdown(self, _how: int) -> None:
                return None

            def recv(self, _limit: int) -> bytes:
                return b"ok\n"

        sock = FakeSocket()
        with mock.patch.object(
            bootstrap_eboot.socket,
            "create_connection",
            return_value=sock,
        ) as connect:
            reply = bootstrap_eboot.send_companion_command(
                "10.1.1.93", "SLRS00001", "kill"
            )
        connect.assert_called_once_with(
            ("10.1.1.93", bootstrap_eboot.COMPANION_PORT),
            timeout=bootstrap_eboot.NETWORK_TIMEOUT_SECONDS,
        )
        self.assertEqual(bootstrap_eboot.COMPANION_PORT, 1338)
        self.assertEqual(sock.sent, b"kill SLRS00001\n")
        self.assertEqual(reply, "ok")

    def test_lifecycle_requires_exact_companion_confirmations(self) -> None:
        harness = Harness(self)
        try:
            harness.manager.command_sender = (
                lambda _ip, _title, _action: bootstrap_eboot.COMPANION_LAUNCH_OK
            )
            self.assertEqual(
                harness.manager._lifecycle("launch"),
                bootstrap_eboot.COMPANION_LAUNCH_OK,
            )
            harness.manager.command_sender = lambda _ip, _title, _action: ""
            with self.assertRaisesRegex(
                bootstrap_eboot.SafetyError, "did not confirm launch"
            ):
                harness.manager._lifecycle("launch")
            harness.manager.command_sender = (
                lambda _ip, _title, _action: "Error: launch queued"
            )
            with self.assertRaisesRegex(
                bootstrap_eboot.SafetyError, "did not confirm launch"
            ):
                harness.manager._lifecycle("launch")
        finally:
            harness.close()

    def test_kill_accepts_only_success_or_v106_already_stopped_reply(self) -> None:
        harness = Harness(self)
        try:
            for accepted in (
                bootstrap_eboot.COMPANION_KILL_OK,
                bootstrap_eboot.COMPANION_KILL_NOT_RUNNING,
            ):
                with self.subTest(accepted=accepted):
                    harness.manager.command_sender = (
                        lambda _ip, _title, _action, reply=accepted: reply
                    )
                    self.assertEqual(harness.manager._lifecycle("kill"), accepted)
            for rejected in ("", "Apps destroyed.", "Killed.\n"):
                with self.subTest(rejected=rejected):
                    harness.manager.command_sender = (
                        lambda _ip, _title, _action, reply=rejected: reply
                    )
                    with self.assertRaisesRegex(
                        bootstrap_eboot.SafetyError, "did not confirm kill"
                    ):
                        harness.manager._lifecycle("kill")
        finally:
            harness.close()

    def test_remote_paths_are_fixed_and_unmanaged_paths_are_refused(self) -> None:
        paths = bootstrap_eboot.RemotePaths.for_title("SLRS00001")
        self.assertEqual(paths.live, "ux0:/app/SLRS00001/eboot.bin")
        self.assertEqual(
            paths.backup,
            "ux0:/app/SLRS00001/eboot.bin.vitadevdeploy.backup",
        )
        with self.assertRaises(bootstrap_eboot.SafetyError):
            paths.require_managed("ux0:/tai/config.txt")

    def test_backup_creates_verified_pc_and_remote_copies_via_part_rename(self) -> None:
        harness = Harness(self)
        try:
            result = harness.manager.backup()
            paths = harness.manager.paths
            self.assertEqual(harness.manager.pc_backup.read_bytes(), harness.original)
            self.assertEqual(harness.ftp.files[paths.backup], harness.original)
            self.assertNotIn(paths.backup_part, harness.ftp.files)
            self.assertIn(("stor", paths.backup_part), harness.ftp.events)
            self.assertIn(("rename", paths.backup_part, paths.backup), harness.ftp.events)
            self.assertEqual(result["vita_backup"]["sha256"], sha(harness.original))
            self.assertEqual(harness.commands, [])
        finally:
            harness.close()

    def test_backup_refuses_wrong_installed_hash_before_writing_backups(self) -> None:
        original = b"known original"
        harness = Harness(self, original=original, live=b"unexpected live eboot")
        try:
            with self.assertRaises(bootstrap_eboot.SafetyError):
                harness.manager.backup()
            self.assertFalse(harness.manager.pc_backup.exists())
            self.assertNotIn(harness.manager.paths.backup, harness.ftp.files)
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
        finally:
            harness.close()

    def test_backup_refuses_to_overwrite_mismatched_remote_backup(self) -> None:
        harness = Harness(self, remote_backup=b"not the original")
        try:
            harness.write_pc_backup()
            with self.assertRaises(bootstrap_eboot.SafetyError):
                harness.manager.backup()
            self.assertEqual(
                harness.ftp.files[harness.manager.paths.backup], b"not the original"
            )
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
        finally:
            harness.close()

    def test_activate_checks_both_backups_stages_kills_commits_and_launches(self) -> None:
        bootstrap = b"verified bootstrap eboot"
        harness = Harness(self, remote_backup=b"original eboot contents")
        try:
            harness.write_pc_backup()
            bootstrap_path = harness.backup_dir / "bootstrap.bin"
            bootstrap_path.write_bytes(bootstrap)
            result = harness.manager.activate(bootstrap_path, sha(bootstrap))
            paths = harness.manager.paths
            self.assertEqual(harness.ftp.files[paths.live], bootstrap)
            self.assertEqual(harness.ftp.files[paths.backup], harness.original)
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill", "launch"]
            )
            stage_index = harness.ftp.events.index(("stor", paths.live_part))
            kill_index = harness.ftp.events.index(("command", "kill", "SLRS00001"))
            rename_index = harness.ftp.events.index(
                ("rename", paths.live_part, paths.live)
            )
            delete_index = harness.ftp.events.index(("delete", paths.live))
            self.assertLess(stage_index, kill_index)
            self.assertLess(kill_index, delete_index)
            self.assertLess(delete_index, rename_index)
            self.assertEqual(
                [event[1] for event in harness.ftp.events if event[0] == "delete"],
                [paths.live],
            )
            self.assertFalse(result["already_active"])
        finally:
            harness.close()

    def test_activate_refuses_bad_bootstrap_hash_before_ftp(self) -> None:
        harness = Harness(self, remote_backup=b"original eboot contents")
        try:
            harness.write_pc_backup()
            bootstrap_path = harness.backup_dir / "bootstrap.bin"
            bootstrap_path.write_bytes(b"bootstrap")
            with self.assertRaises(bootstrap_eboot.SafetyError):
                harness.manager.activate(bootstrap_path, "f" * 64)
            self.assertEqual(harness.ftp.events, [])
            self.assertEqual(harness.commands, [])
        finally:
            harness.close()

    def test_activate_refuses_remote_size_mismatch_without_lifecycle_commands(self) -> None:
        harness = Harness(self, remote_backup=b"original eboot contents")
        try:
            harness.write_pc_backup()
            bootstrap_path = harness.backup_dir / "bootstrap.bin"
            bootstrap_path.write_bytes(b"bootstrap")
            harness.ftp.size_overrides[harness.manager.paths.backup] = len(
                harness.original
            ) + 1
            with self.assertRaises(bootstrap_eboot.SafetyError):
                harness.manager.activate(bootstrap_path, sha(b"bootstrap"))
            self.assertEqual(harness.commands, [])
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
        finally:
            harness.close()

    def test_activate_is_idempotent_for_an_already_active_bootstrap(self) -> None:
        original = b"original"
        active = b"bootstrap"
        harness = Harness(
            self, original=original, live=active, remote_backup=original
        )
        try:
            harness.write_pc_backup()
            bootstrap_path = harness.backup_dir / "bootstrap.bin"
            bootstrap_path.write_bytes(active)
            result = harness.manager.activate(bootstrap_path, sha(active))
            self.assertTrue(result["already_active"])
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill", "launch"]
            )
        finally:
            harness.close()

    def test_activate_commit_failure_recovers_and_relaunches_exact_original(self) -> None:
        original = b"original"
        harness = Harness(self, original=original, remote_backup=original)
        try:
            harness.write_pc_backup()
            bootstrap_path = harness.backup_dir / "bootstrap.bin"
            bootstrap_path.write_bytes(b"bootstrap")
            key = (harness.manager.paths.live_part, harness.manager.paths.live)
            harness.ftp.rename_failures[key] = 1
            with self.assertRaisesRegex(
                bootstrap_eboot.SafetyError, "exact original eboot was recovered"
            ):
                harness.manager.activate(bootstrap_path, sha(b"bootstrap"))
            self.assertEqual(
                harness.ftp.files[harness.manager.paths.live], original
            )
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill", "launch"]
            )
            self.assertEqual(
                [event[1] for event in harness.ftp.events if event[0] == "delete"],
                [harness.manager.paths.live],
            )
        finally:
            harness.close()

    def test_restore_uses_verified_pc_backup_part_rename_and_launches(self) -> None:
        original = b"original"
        harness = Harness(
            self,
            original=original,
            live=b"bootstrap",
            remote_backup=original,
        )
        try:
            harness.write_pc_backup()
            result = harness.manager.restore()
            paths = harness.manager.paths
            self.assertEqual(harness.ftp.files[paths.live], original)
            self.assertEqual(harness.ftp.files[paths.backup], original)
            self.assertNotIn(paths.live_part, harness.ftp.files)
            self.assertIn(("stor", paths.live_part), harness.ftp.events)
            self.assertIn(("rename", paths.live_part, paths.live), harness.ftp.events)
            self.assertEqual(
                [event[1] for event in harness.ftp.events if event[0] == "delete"],
                [paths.live],
            )
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill", "launch"]
            )
            kill_index = harness.ftp.events.index(
                ("command", "kill", "SLRS00001")
            )
            live_reads = [
                index
                for index, event in enumerate(harness.ftp.events)
                if event == ("retr", paths.live)
            ]
            stage_index = harness.ftp.events.index(("stor", paths.live_part))
            self.assertGreaterEqual(len(live_reads), 2)
            self.assertTrue(
                any(kill_index < read_index < stage_index for read_index in live_reads)
            )
            self.assertFalse(result["already_restored"])
        finally:
            harness.close()

    def test_restore_refuses_live_change_after_kill_before_staging(self) -> None:
        original = b"original"
        harness = Harness(
            self,
            original=original,
            live=b"bootstrap",
            remote_backup=original,
        )
        try:
            harness.write_pc_backup()
            original_sender = harness.manager.command_sender

            def mutate_live_on_kill(ip: str, title: str, action: str) -> str:
                reply = original_sender(ip, title, action)
                if action == "kill":
                    harness.ftp.files[harness.manager.paths.live] = b"changed"
                return reply

            harness.manager.command_sender = mutate_live_on_kill
            with self.assertRaisesRegex(
                bootstrap_eboot.SafetyError,
                "installed eboot changed while stopping the title",
            ):
                harness.manager.restore()
            self.assertEqual(
                harness.ftp.files[harness.manager.paths.live], b"changed"
            )
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill"]
            )
        finally:
            harness.close()

    def test_restore_is_idempotent_for_an_already_restored_original(self) -> None:
        original = b"original"
        harness = Harness(
            self,
            original=original,
            live=original,
            remote_backup=original,
        )
        try:
            harness.write_pc_backup()
            result = harness.manager.restore()
            self.assertTrue(result["already_restored"])
            self.assertEqual(harness.ftp.files[harness.manager.paths.live], original)
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
            self.assertEqual(
                [entry[2] for entry in harness.commands], ["kill", "launch"]
            )
        finally:
            harness.close()

    def test_restore_refuses_mismatched_pc_backup_without_touching_live(self) -> None:
        original = b"original"
        harness = Harness(
            self,
            original=original,
            live=b"bootstrap",
            remote_backup=original,
        )
        try:
            harness.write_pc_backup(b"corrupt")
            with self.assertRaises(bootstrap_eboot.SafetyError):
                harness.manager.restore()
            self.assertEqual(harness.ftp.files[harness.manager.paths.live], b"bootstrap")
            self.assertEqual(harness.commands, [])
            self.assertFalse(any(event[0] == "stor" for event in harness.ftp.events))
        finally:
            harness.close()

    def test_status_is_read_only_and_reports_readiness(self) -> None:
        original = b"original"
        harness = Harness(self, original=original, remote_backup=original)
        try:
            harness.write_pc_backup()
            result = harness.manager.status()
            self.assertEqual(result["live_state"], "original")
            self.assertTrue(result["ready_to_activate"])
            self.assertTrue(result["ready_to_restore"])
            self.assertEqual(harness.commands, [])
            self.assertFalse(
                any(event[0] in {"stor", "rename"} for event in harness.ftp.events)
            )
        finally:
            harness.close()


if __name__ == "__main__":
    unittest.main()
