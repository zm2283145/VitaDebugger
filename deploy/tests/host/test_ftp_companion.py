from __future__ import annotations

import ftplib
import io
import posixpath
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.companion import VitaCompanionClient
from vitadevdeploy.errors import DeploymentError
from vitadevdeploy.ftp import DEFAULT_REMOTE_ROOT, VitaFtpClient
from vitadevdeploy.job import load_prepared_job
from vitadevdeploy.manifest import build_manifest
from vitadevdeploy.protocol import make_request
try:
    from .helpers import make_sfo
except ImportError:
    from helpers import make_sfo


class FakeFtp:
    def __init__(self) -> None:
        self.directories = {"/", "/ux0:", "/ux0:/data"}
        self.files: dict[str, bytes] = {}
        self.events: list[tuple[str, str, str | None]] = []
        self.current_directory = "/"
        self.store_commands: list[str] = []

    def _resolve(self, path: str) -> str:
        if path.startswith("/"):
            return posixpath.normpath(path)
        return posixpath.normpath(posixpath.join(self.current_directory, path))

    @staticmethod
    def _require_basename(path: str, command: str) -> None:
        if "/" in path or path in {"", ".", ".."}:
            raise ftplib.error_perm(f"550 {command} requires a basename")

    def connect(self, host: str, port: int, timeout: float) -> None:
        self.events.append(("connect", f"{host}:{port}", None))

    def login(self) -> None:
        self.events.append(("login", "", None))

    def voidcmd(self, command: str) -> str:
        return "200 OK"

    def cwd(self, path: str) -> None:
        resolved = self._resolve(path)
        if resolved not in self.directories:
            raise ftplib.error_perm("550 missing")
        self.current_directory = resolved

    def mkd(self, path: str) -> str:
        self._require_basename(path, "MKD")
        resolved = self._resolve(path)
        self.directories.add(resolved)
        self.events.append(("mkd", resolved, None))
        return resolved

    def storbinary(self, command: str, stream: object) -> str:
        path = command.removeprefix("STOR ")
        self.store_commands.append(path)
        self._require_basename(path, "STOR")
        resolved = self._resolve(path)
        self.files[resolved] = stream.read()  # type: ignore[attr-defined]
        self.events.append(("store", resolved, None))
        return "226 done"

    def size(self, path: str) -> int:
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        return len(self.files[path])

    def delete(self, path: str) -> str:
        self._require_basename(path, "DELE")
        resolved = self._resolve(path)
        if resolved not in self.files:
            raise ftplib.error_perm("550 missing")
        del self.files[resolved]
        self.events.append(("delete", resolved, None))
        return "250 deleted"

    def rename(self, source: str, destination: str) -> str:
        self._require_basename(source, "RNFR")
        self._require_basename(destination, "RNTO")
        resolved_source = self._resolve(source)
        resolved_destination = self._resolve(destination)
        self.files[resolved_destination] = self.files.pop(resolved_source)
        self.events.append(("rename", resolved_source, resolved_destination))
        return "250 renamed"

    def retrbinary(self, command: str, callback: object) -> str:
        path = command.removeprefix("RETR ")
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        callback(self.files[path])  # type: ignore[operator]
        return "226 done"

    def quit(self) -> None:
        self.events.append(("quit", "", None))

    def close(self) -> None:
        pass


def make_unsigned_job(root: Path) -> Path:
    job_id = "12" * 16
    job_root = root / job_id
    package = job_root / "package"
    (package / "sce_sys" / "package").mkdir(parents=True)
    (package / "eboot.bin").write_bytes(b"eboot")
    (package / "sce_sys" / "param.sfo").write_bytes(make_sfo())
    (package / "sce_sys" / "package" / "head.bin").write_bytes(b"head")
    manifest = build_manifest(package)
    request = make_request(
        job=job_id,
        nonce="ab" * 32,
        action="verify",
        title_id="TEST00001",
        manifest_sha256=manifest.sha256,
        file_count=manifest.file_count,
        total_size=manifest.total_size,
    )
    (job_root / "manifest.v1").write_bytes(manifest.data)
    (job_root / "request.v1").write_bytes(request.data)
    (job_root / "signature.bin").write_bytes(b"s" * 64)
    return job_root


class FtpTests(unittest.TestCase):
    def test_fake_models_v106_absolute_stor_rejection(self) -> None:
        fake = FakeFtp()
        with self.assertRaises(ftplib.error_perm):
            fake.storbinary(
                f"STOR {DEFAULT_REMOTE_ROOT}/inbox/absolute.part",
                io.BytesIO(b"bad"),
            )

    def test_stage_job_uses_parent_relative_stor_commands(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeFtp()
            job = load_prepared_job(make_unsigned_job(Path(directory)))
            client = VitaFtpClient("vita", ftp_factory=lambda: fake)
            with client:
                client.stage_job(job)
            self.assertTrue(fake.store_commands)
            self.assertTrue(all("/" not in operand for operand in fake.store_commands))

    def test_replacing_uncommitted_file_uses_parent_relative_delete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeFtp()
            job = load_prepared_job(make_unsigned_job(Path(directory)))
            remote_job = f"{DEFAULT_REMOTE_ROOT}/inbox/{job.request.job}"
            fake.files[f"{remote_job}/manifest.v1"] = b"old manifest"
            client = VitaFtpClient("vita", ftp_factory=lambda: fake)
            with client:
                client.stage_job(job)
            deletes = [event for event in fake.events if event[0] == "delete"]
            self.assertEqual(deletes, [("delete", f"{remote_job}/manifest.v1", None)])

    def test_request_rename_is_final_commit_operation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeFtp()
            job = load_prepared_job(make_unsigned_job(Path(directory)))
            client = VitaFtpClient("vita", ftp_factory=lambda: fake)
            with client:
                remote = client.stage_job(job)
            self.assertEqual(remote, f"{DEFAULT_REMOTE_ROOT}/inbox/{job.request.job}")
            renames = [event for event in fake.events if event[0] == "rename"]
            self.assertTrue(renames)
            self.assertEqual(renames[-1][2], f"{remote}/request.v1")
            self.assertNotIn(f"{remote}/request.v1.part", fake.files)

    def test_refuses_second_commit_for_same_job(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeFtp()
            job = load_prepared_job(make_unsigned_job(Path(directory)))
            client = VitaFtpClient("vita", ftp_factory=lambda: fake)
            with client:
                client.stage_job(job)
                with self.assertRaises(DeploymentError):
                    client.stage_job(job)

    def test_read_result_is_nonpolling_and_optional(self) -> None:
        fake = FakeFtp()
        job_id = "34" * 16
        client = VitaFtpClient("vita", ftp_factory=lambda: fake)
        with client:
            self.assertIsNone(client.read_result(job_id))
            remote = f"{DEFAULT_REMOTE_ROOT}/results/{job_id}.result"
            fake.files[remote] = (
                "VITADEVDEPLOY-RESULT-1\n"
                f"job={job_id}\n"
                "state=success\n"
                "stage=complete\n"
                "code=0\n"
                "title_id=TEST00001\n"
                "message=ok\n"
            ).encode("ascii")
            result = client.read_result(job_id)
        self.assertIsNotNone(result)
        self.assertTrue(result.succeeded)  # type: ignore[union-attr]

    def test_optional_read_remains_confined_to_deployment_root(self) -> None:
        fake = FakeFtp()
        client = VitaFtpClient("vita", ftp_factory=lambda: fake)
        with client:
            with self.assertRaises(DeploymentError):
                client.read_optional_bytes("/ux0:/app/TEST00001/eboot.bin", max_size=1)

    def test_optional_read_fails_closed_on_permission_error(self) -> None:
        class PermissionDeniedFtp(FakeFtp):
            def retrbinary(self, command: str, callback: object) -> str:
                raise ftplib.error_perm("550 Permission denied")

        fake = PermissionDeniedFtp()
        client = VitaFtpClient("vita", ftp_factory=lambda: fake)
        with client:
            with self.assertRaises(DeploymentError):
                client.read_optional_bytes(
                    f"{DEFAULT_REMOTE_ROOT}/promote.state", max_size=1024
                )


class CompanionTests(unittest.TestCase):
    def _server(self, response: bytes) -> tuple[int, list[bytes], threading.Thread]:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        received: list[bytes] = []

        def serve() -> None:
            try:
                connection, _ = listener.accept()
                with connection:
                    received.append(connection.recv(256))
                    connection.sendall(response)
            finally:
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        return listener.getsockname()[1], received, thread

    def test_launch_uses_validated_single_command(self) -> None:
        port, received, thread = self._server(b"Launched.\n")
        client = VitaCompanionClient("127.0.0.1", port=port, timeout=2)
        self.assertEqual(client.launch("TEST00001"), "Launched.")
        thread.join(2)
        self.assertEqual(received, [b"launch TEST00001\n"])

    def test_destroy_requires_exact_success_and_uses_single_command(self) -> None:
        port, received, thread = self._server(b"Apps destroyed.\n")
        client = VitaCompanionClient("127.0.0.1", port=port, timeout=2)
        self.assertEqual(client.destroy(), "Apps destroyed.")
        thread.join(2)
        self.assertEqual(received, [b"destroy\n"])

    def test_destroy_rejects_non_success_reply(self) -> None:
        port, _received, thread = self._server(b"Error: destroy failed\n")
        client = VitaCompanionClient("127.0.0.1", port=port, timeout=2)
        with self.assertRaises(DeploymentError):
            client.destroy()
        thread.join(2)

    def test_rejects_command_chaining(self) -> None:
        client = VitaCompanionClient("127.0.0.1")
        with self.assertRaises(DeploymentError):
            client.command("launch TEST00001; reboot")


if __name__ == "__main__":
    unittest.main()
