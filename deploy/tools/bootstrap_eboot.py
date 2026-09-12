#!/usr/bin/env python3
"""Safely swap the SLRS00001 eboot for the VitaDevDeploy bootstrap.

The expected SHA-256 supplied to every command is always the hash of the exact
original eboot.  ``activate`` additionally requires the expected SHA-256 of the
bootstrap eboot.  Remote paths are derived from the one supported title and
cannot be supplied by callers.
"""

from __future__ import annotations

import argparse
import contextlib
import ftplib
import hashlib
import ipaddress
import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path
import re
import socket
import sys
import time
from typing import Callable, Iterator, Optional


FTP_PORT = 1337
COMPANION_PORT = 1338
NETWORK_TIMEOUT_SECONDS = 10.0
KILL_SETTLE_SECONDS = 0.5
SUPPORTED_TITLE = "SLRS00001"
TITLE_PATTERN = re.compile(r"[A-Z0-9]{9}\Z")
SHA256_PATTERN = re.compile(r"[0-9a-fA-F]{64}\Z")
CHUNK_SIZE = 64 * 1024
COMPANION_LAUNCH_OK = "Launched."
COMPANION_KILL_OK = "Killed."
COMPANION_KILL_NOT_RUNNING = "Error: cannot kill the app. Is the TITLEID correct?"


class SafetyError(RuntimeError):
    """Raised when an operation cannot prove that it is safe to continue."""


@dataclass(frozen=True)
class FileDigest:
    path: str
    size: int
    sha256: str


@dataclass(frozen=True)
class RemotePaths:
    live: str
    backup: str
    live_part: str
    backup_part: str

    @classmethod
    def for_title(cls, title: str) -> "RemotePaths":
        checked = validate_title(title)
        live = f"ux0:/app/{checked}/eboot.bin"
        backup = f"{live}.vitadevdeploy.backup"
        return cls(
            live=live,
            backup=backup,
            live_part=f"{live}.vitadevdeploy.part",
            backup_part=f"{backup}.part",
        )

    def require_managed(self, path: str) -> None:
        if path not in {self.live, self.backup, self.live_part, self.backup_part}:
            raise SafetyError(f"refusing unmanaged Vita path: {path!r}")


def validate_title(value: str) -> str:
    if TITLE_PATTERN.fullmatch(value) is None:
        raise SafetyError("title must match [A-Z0-9]{9}")
    if value != SUPPORTED_TITLE:
        raise SafetyError(
            f"this bootstrap helper is pinned to {SUPPORTED_TITLE}; got {value}"
        )
    return value


def validate_sha256(value: str, label: str = "SHA-256") -> str:
    if SHA256_PATTERN.fullmatch(value) is None:
        raise SafetyError(f"{label} must contain exactly 64 hexadecimal characters")
    return value.lower()


def validate_vita_ip(value: str) -> str:
    try:
        parsed = ipaddress.ip_address(value)
    except ValueError as exc:
        raise SafetyError("Vita address must be an explicit IPv4 address") from exc
    if parsed.version != 4:
        raise SafetyError("Vita address must be an explicit IPv4 address")
    return str(parsed)


def digest_file(path: Path) -> FileDigest:
    try:
        before_size = path.stat().st_size
        if not path.is_file():
            raise SafetyError(f"not a regular file: {path}")
        digest = hashlib.sha256()
        transferred = 0
        with path.open("rb") as source:
            while True:
                chunk = source.read(CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
                transferred += len(chunk)
    except FileNotFoundError as exc:
        raise SafetyError(f"required local file is missing: {path}") from exc
    if transferred != before_size:
        raise SafetyError(f"local file size changed while reading: {path}")
    return FileDigest(str(path), transferred, digest.hexdigest())


def send_companion_command(
    vita_ip: str,
    title: str,
    action: str,
    *,
    timeout: float = NETWORK_TIMEOUT_SECONDS,
) -> str:
    """Send one title-scoped lifecycle command to Vita Companion port 1338."""

    host = validate_vita_ip(vita_ip)
    checked_title = validate_title(title)
    if action not in {"kill", "launch"}:
        raise SafetyError(f"unsupported Vita Companion action: {action!r}")
    payload = f"{action} {checked_title}\n".encode("ascii")
    with socket.create_connection((host, COMPANION_PORT), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        with contextlib.suppress(OSError):
            sock.shutdown(socket.SHUT_WR)
        try:
            reply = sock.recv(64 * 1024)
        except socket.timeout:
            reply = b""
    return reply.decode("utf-8", errors="replace").strip()


class BootstrapEbootManager:
    """Hash-checked bootstrap activation and restoration for one Vita title."""

    def __init__(
        self,
        vita_ip: str,
        title: str,
        expected_original_sha256: str,
        *,
        backup_dir: Optional[Path] = None,
        ftp_factory: Callable[[], ftplib.FTP] = ftplib.FTP,
        command_sender: Callable[[str, str, str], str] = send_companion_command,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        self.vita_ip = validate_vita_ip(vita_ip)
        self.title = validate_title(title)
        self.expected_original_sha256 = validate_sha256(
            expected_original_sha256, "expected original SHA-256"
        )
        self.paths = RemotePaths.for_title(self.title)
        project_root = Path(__file__).resolve().parents[1]
        self.backup_dir = Path(backup_dir) if backup_dir else project_root / "local" / "bootstrap-backup"
        self.pc_backup = self.backup_dir / f"{self.title}-eboot.bin"
        self.pc_part = self.backup_dir / f"{self.title}-eboot.bin.part"
        self.ftp_factory = ftp_factory
        self.command_sender = command_sender
        self.sleeper = sleeper

    @contextlib.contextmanager
    def _ftp(self) -> Iterator[ftplib.FTP]:
        ftp = self.ftp_factory()
        try:
            ftp.connect(self.vita_ip, FTP_PORT, timeout=NETWORK_TIMEOUT_SECONDS)
            ftp.login()
            ftp.voidcmd("TYPE I")
            yield ftp
        finally:
            try:
                ftp.quit()
            except Exception:
                with contextlib.suppress(Exception):
                    ftp.close()

    def _remote_size(self, ftp: ftplib.FTP, path: str) -> Optional[int]:
        self.paths.require_managed(path)
        try:
            size = ftp.size(path)
        except ftplib.error_perm as exc:
            if str(exc).lstrip().startswith("550"):
                return None
            raise
        if size is None or not isinstance(size, int) or size < 0:
            raise SafetyError(f"Vita returned an invalid size for {path!r}: {size!r}")
        return size

    def _remote_digest(
        self, ftp: ftplib.FTP, path: str, *, required: bool = True
    ) -> Optional[FileDigest]:
        self.paths.require_managed(path)
        advertised_size = self._remote_size(ftp, path)
        if advertised_size is None:
            if required:
                raise SafetyError(f"required Vita file is missing: {path}")
            return None

        digest = hashlib.sha256()
        transferred = 0

        def consume(chunk: bytes) -> None:
            nonlocal transferred
            digest.update(chunk)
            transferred += len(chunk)

        ftp.retrbinary(f"RETR {path}", consume, blocksize=CHUNK_SIZE)
        if transferred != advertised_size:
            raise SafetyError(
                f"Vita size mismatch for {path}: SIZE reported {advertised_size}, "
                f"download returned {transferred}"
            )
        return FileDigest(path, transferred, digest.hexdigest())

    def _download_remote(self, ftp: ftplib.FTP, path: str, destination: Path) -> FileDigest:
        self.paths.require_managed(path)
        advertised_size = self._remote_size(ftp, path)
        if advertised_size is None:
            raise SafetyError(f"required Vita file is missing: {path}")
        digest = hashlib.sha256()
        transferred = 0
        with destination.open("wb") as output:

            def consume(chunk: bytes) -> None:
                nonlocal transferred
                output.write(chunk)
                digest.update(chunk)
                transferred += len(chunk)

            ftp.retrbinary(f"RETR {path}", consume, blocksize=CHUNK_SIZE)
            output.flush()
            os.fsync(output.fileno())
        if transferred != advertised_size:
            raise SafetyError(
                f"Vita size mismatch for {path}: SIZE reported {advertised_size}, "
                f"download returned {transferred}"
            )
        return FileDigest(path, transferred, digest.hexdigest())

    @staticmethod
    def _require_hash(info: FileDigest, expected_sha256: str, label: str) -> None:
        if info.sha256 != expected_sha256:
            raise SafetyError(
                f"{label} hash mismatch: expected {expected_sha256}, got {info.sha256}"
            )

    @staticmethod
    def _require_same_file(left: FileDigest, right: FileDigest, label: str) -> None:
        if left.size != right.size:
            raise SafetyError(
                f"{label} size mismatch: {left.path} is {left.size} bytes, "
                f"{right.path} is {right.size} bytes"
            )
        if left.sha256 != right.sha256:
            raise SafetyError(
                f"{label} hash mismatch: {left.path} and {right.path} differ"
            )

    def _stage_upload(
        self,
        ftp: ftplib.FTP,
        source: Path,
        source_info: FileDigest,
        part_path: str,
    ) -> FileDigest:
        self.paths.require_managed(part_path)
        with source.open("rb") as input_file:
            ftp.storbinary(f"STOR {part_path}", input_file, blocksize=CHUNK_SIZE)
        staged = self._remote_digest(ftp, part_path)
        assert staged is not None
        self._require_same_file(source_info, staged, "staged upload")
        return staged

    def _commit_stage(
        self,
        ftp: ftplib.FTP,
        part_path: str,
        target_path: str,
        expected: FileDigest,
        *,
        replace_live: bool = False,
    ) -> FileDigest:
        self.paths.require_managed(part_path)
        self.paths.require_managed(target_path)
        if replace_live:
            if target_path != self.paths.live:
                raise SafetyError("replacement deletion is permitted only for the live eboot")
            # sceIoRename, used by Vita Companion's RNTO implementation, returns
            # EEXIST.  The already-killed, fixed live path is therefore removed
            # immediately before the verified temporary file is renamed.
            ftp.delete(target_path)
            if self._remote_size(ftp, target_path) is not None:
                raise SafetyError(f"live eboot still exists after FTP delete: {target_path}")
        ftp.rename(part_path, target_path)
        committed = self._remote_digest(ftp, target_path)
        assert committed is not None
        self._require_same_file(expected, committed, "committed upload")
        if self._remote_size(ftp, part_path) is not None:
            raise SafetyError(f"FTP rename left its temporary source behind: {part_path}")
        return committed

    def _recover_original(self, ftp: ftplib.FTP, pc: FileDigest) -> FileDigest:
        """Best-effort recovery after a live commit failure, using only fixed paths."""

        current = self._remote_digest(ftp, self.paths.live, required=False)
        if current is not None and current.sha256 == self.expected_original_sha256:
            self._require_same_file(pc, current, "recovered original")
            return current

        staged = self._stage_upload(ftp, self.pc_backup, pc, self.paths.live_part)
        return self._commit_stage(
            ftp,
            self.paths.live_part,
            self.paths.live,
            staged,
            replace_live=current is not None,
        )

    def _verified_backups(self, ftp: ftplib.FTP) -> tuple[FileDigest, FileDigest]:
        pc = digest_file(self.pc_backup)
        self._require_hash(pc, self.expected_original_sha256, "PC backup")
        remote = self._remote_digest(ftp, self.paths.backup)
        assert remote is not None
        self._require_hash(remote, self.expected_original_sha256, "Vita backup")
        self._require_same_file(pc, remote, "PC/Vita backup")
        return pc, remote

    def _lifecycle(self, action: str) -> str:
        reply = self.command_sender(self.vita_ip, self.title, action)
        if action == "launch":
            if reply != COMPANION_LAUNCH_OK:
                raise SafetyError(
                    f"Vita Companion did not confirm launch; reply was {reply!r}"
                )
            return reply
        if action == "kill":
            # Vita Companion 1.06 uses the same error text for an invalid title
            # and a valid title that is not currently running.  This manager's
            # title is syntax-checked and pinned to SLRS00001, so that one exact
            # response is treated as the benign already-stopped case.
            if reply not in {COMPANION_KILL_OK, COMPANION_KILL_NOT_RUNNING}:
                raise SafetyError(
                    f"Vita Companion did not confirm kill; reply was {reply!r}"
                )
            return reply
        raise SafetyError(f"unsupported lifecycle action: {action!r}")

    def backup(self) -> dict[str, object]:
        """Create and independently verify PC and Vita copies of the original."""

        self.backup_dir.mkdir(parents=True, exist_ok=True)
        try:
            with self._ftp() as ftp:
                captured = self._download_remote(ftp, self.paths.live, self.pc_part)
                self._require_hash(
                    captured, self.expected_original_sha256, "installed original eboot"
                )

                if self.pc_backup.exists():
                    pc = digest_file(self.pc_backup)
                    self._require_hash(pc, self.expected_original_sha256, "existing PC backup")
                    self._require_same_file(captured, pc, "installed/PC original")
                else:
                    os.replace(self.pc_part, self.pc_backup)
                    pc = digest_file(self.pc_backup)
                    self._require_same_file(captured, pc, "saved PC backup")

                remote = self._remote_digest(ftp, self.paths.backup, required=False)
                if remote is None:
                    staged = self._stage_upload(
                        ftp, self.pc_backup, pc, self.paths.backup_part
                    )
                    remote = self._commit_stage(
                        ftp, self.paths.backup_part, self.paths.backup, staged
                    )
                else:
                    self._require_hash(
                        remote, self.expected_original_sha256, "existing Vita backup"
                    )
                    self._require_same_file(pc, remote, "PC/Vita backup")

                final_pc, final_remote = self._verified_backups(ftp)
                return {
                    "operation": "backup",
                    "title": self.title,
                    "pc_backup": asdict(final_pc),
                    "vita_backup": asdict(final_remote),
                }
        finally:
            # This is the sole local temporary name, derived from the validated title.
            with contextlib.suppress(FileNotFoundError):
                self.pc_part.unlink()

    def activate(self, bootstrap: Path, expected_bootstrap_sha256: str) -> dict[str, object]:
        """Replace the live eboot with a fully verified bootstrap and launch it."""

        bootstrap_path = Path(bootstrap)
        expected_bootstrap = validate_sha256(
            expected_bootstrap_sha256, "expected bootstrap SHA-256"
        )
        bootstrap_info = digest_file(bootstrap_path)
        self._require_hash(bootstrap_info, expected_bootstrap, "bootstrap eboot")
        if bootstrap_info.sha256 == self.expected_original_sha256:
            raise SafetyError("bootstrap and original hashes are identical")

        already_active = False
        committed: Optional[FileDigest] = None
        with self._ftp() as ftp:
            pc, _ = self._verified_backups(ftp)
            live = self._remote_digest(ftp, self.paths.live)
            assert live is not None

            if live.sha256 == bootstrap_info.sha256:
                self._require_same_file(bootstrap_info, live, "active bootstrap")
                already_active = True
            else:
                self._require_hash(
                    live, self.expected_original_sha256, "installed eboot before activation"
                )
                self._require_same_file(pc, live, "installed/backup original")
                staged = self._stage_upload(
                    ftp, bootstrap_path, bootstrap_info, self.paths.live_part
                )
                live_before_commit = self._remote_digest(ftp, self.paths.live)
                assert live_before_commit is not None
                self._require_same_file(
                    pc, live_before_commit, "installed original changed during staging"
                )

            self._lifecycle("kill")
            self.sleeper(KILL_SETTLE_SECONDS)
            if not already_active:
                try:
                    live_after_kill = self._remote_digest(ftp, self.paths.live)
                    assert live_after_kill is not None
                    self._require_same_file(
                        pc, live_after_kill, "installed original changed before commit"
                    )
                    committed = self._commit_stage(
                        ftp,
                        self.paths.live_part,
                        self.paths.live,
                        staged,
                        replace_live=True,
                    )
                except Exception as commit_error:
                    try:
                        self._recover_original(ftp, pc)
                        self._lifecycle("launch")
                    except Exception as recovery_error:
                        raise SafetyError(
                            "bootstrap activation failed and automatic original-eboot "
                            f"recovery also failed: {recovery_error}"
                        ) from recovery_error
                    raise SafetyError(
                        "bootstrap activation failed; the exact original eboot was "
                        "recovered and relaunched"
                    ) from commit_error
            else:
                committed = live

        launch_reply = self._lifecycle("launch")
        assert committed is not None
        return {
            "operation": "activate",
            "title": self.title,
            "already_active": already_active,
            "live": asdict(committed),
            "launch_reply": launch_reply,
        }

    def restore(self) -> dict[str, object]:
        """Restore the exact, independently verified original eboot and launch it."""

        already_restored = False
        recovered_after_commit_failure = False
        committed: Optional[FileDigest] = None
        with self._ftp() as ftp:
            pc, _ = self._verified_backups(ftp)
            live = self._remote_digest(ftp, self.paths.live, required=False)
            if live is not None and live.sha256 == self.expected_original_sha256:
                self._require_same_file(pc, live, "installed/restored original")
                already_restored = True
                committed = live

            self._lifecycle("kill")
            self.sleeper(KILL_SETTLE_SECONDS)
            if not already_restored:
                live_after_kill = self._remote_digest(
                    ftp, self.paths.live, required=False
                )
                if live is None:
                    if live_after_kill is not None:
                        raise SafetyError(
                            "installed eboot appeared after its pre-kill verification"
                        )
                else:
                    if live_after_kill is None:
                        raise SafetyError(
                            "installed eboot disappeared after its pre-kill verification"
                        )
                    self._require_same_file(
                        live,
                        live_after_kill,
                        "installed eboot changed while stopping the title",
                    )

                # A running title may keep its app directory locked against FTP
                # writes.  Stage only after Companion has stopped the exact title
                # and the live eboot has been re-read and proven unchanged.
                staged = self._stage_upload(
                    ftp, self.pc_backup, pc, self.paths.live_part
                )
                try:
                    committed = self._commit_stage(
                        ftp,
                        self.paths.live_part,
                        self.paths.live,
                        staged,
                        replace_live=live_after_kill is not None,
                    )
                except Exception as commit_error:
                    try:
                        committed = self._recover_original(ftp, pc)
                        recovered_after_commit_failure = True
                    except Exception as recovery_error:
                        raise SafetyError(
                            "original-eboot restore failed and its recovery attempt "
                            f"also failed: {recovery_error}"
                        ) from recovery_error

        launch_reply = self._lifecycle("launch")
        assert committed is not None
        return {
            "operation": "restore",
            "title": self.title,
            "already_restored": already_restored,
            "recovered_after_commit_failure": recovered_after_commit_failure,
            "live": asdict(committed),
            "launch_reply": launch_reply,
        }

    def status(self) -> dict[str, object]:
        """Inspect all three copies without changing files or application state."""

        pc: Optional[FileDigest]
        if self.pc_backup.exists():
            pc = digest_file(self.pc_backup)
        else:
            pc = None
        with self._ftp() as ftp:
            live = self._remote_digest(ftp, self.paths.live, required=False)
            remote = self._remote_digest(ftp, self.paths.backup, required=False)

        pc_valid = pc is not None and pc.sha256 == self.expected_original_sha256
        remote_valid = remote is not None and remote.sha256 == self.expected_original_sha256
        backups_match = (
            pc is not None
            and remote is not None
            and pc.size == remote.size
            and pc.sha256 == remote.sha256
        )
        if live is None:
            live_state = "missing"
        elif live.sha256 == self.expected_original_sha256:
            live_state = "original"
        else:
            live_state = "modified"
        return {
            "operation": "status",
            "title": self.title,
            "expected_original_sha256": self.expected_original_sha256,
            "live_state": live_state,
            "ready_to_activate": bool(
                pc_valid and remote_valid and backups_match and live_state == "original"
            ),
            "ready_to_restore": bool(pc_valid and remote_valid and backups_match),
            "live": asdict(live) if live else None,
            "pc_backup": asdict(pc) if pc else None,
            "vita_backup": asdict(remote) if remote else None,
        }


def _arg_title(value: str) -> str:
    try:
        return validate_title(value)
    except SafetyError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def _arg_sha256(value: str) -> str:
    try:
        return validate_sha256(value)
    except SafetyError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def _arg_ip(value: str) -> str:
    try:
        return validate_vita_ip(value)
    except SafetyError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Temporarily run a bootstrap from SLRS00001 and restore its exact "
            "original eboot. The expected hash is always the original eboot hash."
        )
    )
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--vita-ip", required=True, type=_arg_ip)
    common.add_argument("--title", required=True, type=_arg_title)
    common.add_argument("--expected-sha256", required=True, type=_arg_sha256)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("backup", parents=[common], help="verify and save both backups")
    activate = commands.add_parser(
        "activate", parents=[common], help="activate and launch a bootstrap eboot"
    )
    activate.add_argument("--bootstrap", required=True, type=Path)
    activate.add_argument("--bootstrap-sha256", required=True, type=_arg_sha256)
    commands.add_parser("restore", parents=[common], help="restore and launch the original")
    commands.add_parser("status", parents=[common], help="read-only integrity status")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    manager = BootstrapEbootManager(
        args.vita_ip, args.title, args.expected_sha256
    )
    try:
        if args.command == "backup":
            result = manager.backup()
        elif args.command == "activate":
            result = manager.activate(args.bootstrap, args.bootstrap_sha256)
        elif args.command == "restore":
            result = manager.restore()
        else:
            result = manager.status()
    except SafetyError as exc:
        print(f"SAFETY REFUSAL: {exc}", file=sys.stderr)
        return 2
    except (ftplib.Error, OSError) as exc:
        print(f"TRANSFER/LIFECYCLE FAILURE: {exc}", file=sys.stderr)
        return 3
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
