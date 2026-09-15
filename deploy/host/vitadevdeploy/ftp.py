"""Atomic FTP staging and result polling for Vita Companion."""

from __future__ import annotations

import ftplib
import io
import os
import time
from pathlib import Path
from typing import Callable

from .errors import DeploymentError, ProtocolError
from .job import PreparedJob, load_prepared_job
from .manifest import verify_manifest
from .paths import normalize_package_path, validate_job_id
from .protocol import Challenge, Result, parse_challenge, parse_result

DEFAULT_REMOTE_ROOT = "/ux0:/data/VitaDevDeploy"
_REMOTE_ROOT_PARENT = "/ux0:/data"
_REMOTE_ROOT_NAME = "VitaDevDeploy"


class VitaFtpClient:
    def __init__(
        self,
        host: str,
        *,
        port: int = 1337,
        timeout: float = 10.0,
        ftp_factory: Callable[[], ftplib.FTP] = ftplib.FTP,
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._factory = ftp_factory
        self._ftp: ftplib.FTP | None = None

    def connect(self) -> "VitaFtpClient":
        self.close()
        try:
            ftp = self._factory()
            ftp.connect(self.host, self.port, timeout=self.timeout)
            ftp.login()
            ftp.voidcmd("TYPE I")
            self._ftp = ftp
            return self
        except (OSError, ftplib.Error) as exc:
            raise DeploymentError(f"could not connect to Vita Companion FTP: {exc}") from exc

    def close(self) -> None:
        ftp, self._ftp = self._ftp, None
        if ftp is None:
            return
        try:
            ftp.quit()
        except (OSError, ftplib.Error):
            try:
                ftp.close()
            except OSError:
                pass

    def __enter__(self) -> "VitaFtpClient":
        return self.connect()

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @property
    def ftp(self) -> ftplib.FTP:
        if self._ftp is None:
            raise DeploymentError("FTP client is not connected")
        return self._ftp

    @staticmethod
    def _validate_remote_path(path: str) -> str:
        """Return a canonical path confined to the deployment root."""

        if not isinstance(path, str):
            raise DeploymentError("remote path must be text")
        if path != DEFAULT_REMOTE_ROOT and not path.startswith(DEFAULT_REMOTE_ROOT + "/"):
            raise DeploymentError("refusing to access a path outside the deployment root")
        if "\\" in path or any(ord(character) < 0x20 or ord(character) == 0x7F for character in path):
            raise DeploymentError("remote path contains a forbidden character")
        components = path.split("/")[1:]
        if any(component in {"", ".", ".."} for component in components):
            raise DeploymentError("remote path is not canonical")
        return path

    @classmethod
    def _parent_and_name(cls, path: str) -> tuple[str, str]:
        path = cls._validate_remote_path(path)
        if path == DEFAULT_REMOTE_ROOT:
            raise DeploymentError("remote file must be inside the deployment root")
        parent, separator, name = path.rpartition("/")
        if not separator or not parent or not name or "/" in name:
            raise DeploymentError("remote file path has no safe basename")
        return parent, name

    def _ensure_directory(self, path: str) -> None:
        path = self._validate_remote_path(path)

        # Vita Companion 1.06 accepts absolute CWD paths, but mutations are
        # reliable only when sent as basenames in the current directory.
        try:
            self.ftp.cwd(DEFAULT_REMOTE_ROOT)
        except ftplib.error_perm:
            try:
                self.ftp.cwd(_REMOTE_ROOT_PARENT)
                self.ftp.mkd(_REMOTE_ROOT_NAME)
                self.ftp.cwd(DEFAULT_REMOTE_ROOT)
            except ftplib.Error as exc:
                raise DeploymentError(f"could not create remote directory {DEFAULT_REMOTE_ROOT}: {exc}") from exc

        current = DEFAULT_REMOTE_ROOT
        relative = path[len(DEFAULT_REMOTE_ROOT) :].lstrip("/")
        for component in (relative.split("/") if relative else ()):
            candidate = f"{current}/{component}"
            try:
                self.ftp.cwd(candidate)
            except ftplib.error_perm:
                try:
                    self.ftp.cwd(current)
                    self.ftp.mkd(component)
                    self.ftp.cwd(candidate)
                except ftplib.Error as exc:
                    raise DeploymentError(f"could not create remote directory {candidate}: {exc}") from exc
            current = candidate

    def _size(self, path: str) -> int | None:
        self._validate_remote_path(path)
        try:
            value = self.ftp.size(path)
            return None if value is None else int(value)
        except ftplib.error_perm:
            return None

    def _upload_stream_atomic(
        self,
        stream: object,
        size: int,
        final_path: str,
        *,
        replace_uncommitted: bool,
    ) -> None:
        part_path = final_path + ".part"
        parent, final_name = self._parent_and_name(final_path)
        part_parent, part_name = self._parent_and_name(part_path)
        if part_parent != parent:
            raise DeploymentError("temporary and final upload paths must share a parent directory")
        try:
            self.ftp.cwd(parent)
            self.ftp.storbinary(f"STOR {part_name}", stream)  # type: ignore[arg-type]
            if self._size(part_path) != size:
                raise DeploymentError(f"remote size check failed for {part_path}")
            if self._size(final_path) is not None:
                if not replace_uncommitted:
                    raise DeploymentError(f"deployment request is already committed: {final_path}")
                self.ftp.cwd(parent)
                self.ftp.delete(final_name)
            self.ftp.cwd(parent)
            self.ftp.rename(part_name, final_name)
            if self._size(final_path) != size:
                raise DeploymentError(f"remote size check failed after rename: {final_path}")
        except (OSError, ftplib.Error) as exc:
            raise DeploymentError(f"FTP upload failed for {final_path}: {exc}") from exc

    def _upload_file_atomic(self, local: Path, remote: str, *, replace_uncommitted: bool = True) -> None:
        with local.open("rb") as stream:
            self._upload_stream_atomic(stream, local.stat().st_size, remote, replace_uncommitted=replace_uncommitted)

    def _upload_bytes_atomic(self, data: bytes, remote: str, *, replace_uncommitted: bool = True) -> None:
        self._upload_stream_atomic(io.BytesIO(data), len(data), remote, replace_uncommitted=replace_uncommitted)

    def stage_job(self, job_or_path: PreparedJob | os.PathLike[str] | str) -> str:
        job = job_or_path if isinstance(job_or_path, PreparedJob) else load_prepared_job(job_or_path)
        validate_job_id(job.request.job)
        verify_manifest(job.package, job.manifest_path.read_bytes())
        signature = job.signature_path.read_bytes()
        if len(signature) != 64:
            raise ProtocolError("signature.bin must contain exactly 64 raw bytes")
        remote_job = f"{DEFAULT_REMOTE_ROOT}/inbox/{job.request.job}"
        if self._size(f"{remote_job}/request.v1") is not None:
            raise DeploymentError(f"deployment job is already committed: {job.request.job}")
        self._ensure_directory(f"{remote_job}/package")

        for entry in job.manifest.entries:
            normalized = normalize_package_path(entry.path)
            if normalized.value != entry.path:
                raise ProtocolError("manifest contains a noncanonical path")
            parent = entry.path.rsplit("/", 1)[0] if "/" in entry.path else ""
            if parent:
                self._ensure_directory(f"{remote_job}/package/{parent}")
            self._upload_file_atomic(job.package.joinpath(*entry.path.split("/")), f"{remote_job}/package/{entry.path}")

        self._upload_file_atomic(job.manifest_path, f"{remote_job}/manifest.v1")
        self._upload_file_atomic(job.signature_path, f"{remote_job}/signature.bin")
        # This rename is the sole commit point and must remain the final upload.
        self._upload_file_atomic(
            job.request_path,
            f"{remote_job}/request.v1",
            replace_uncommitted=False,
        )
        return remote_job

    def read_bytes(self, remote_path: str, *, max_size: int) -> bytes:
        self._validate_remote_path(remote_path)
        chunks: list[bytes] = []
        size = 0

        def collect(block: bytes) -> None:
            nonlocal size
            size += len(block)
            if size > max_size:
                raise DeploymentError(f"remote file exceeds {max_size} bytes: {remote_path}")
            chunks.append(block)

        try:
            self.ftp.retrbinary(f"RETR {remote_path}", collect)
        except DeploymentError:
            raise
        except (OSError, ftplib.Error) as exc:
            raise DeploymentError(f"could not read {remote_path}: {exc}") from exc
        return b"".join(chunks)

    def read_challenge(self) -> Challenge:
        return parse_challenge(self.read_bytes(f"{DEFAULT_REMOTE_ROOT}/challenge.v1", max_size=256))

    def wait_for_challenge(
        self,
        *,
        previous_nonce: str | None = None,
        timeout: float = 15.0,
        poll_interval: float = 0.25,
    ) -> Challenge:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                challenge = self.read_challenge()
                if challenge.nonce != previous_nonce:
                    return challenge
            except (DeploymentError, ProtocolError) as exc:
                last_error = exc
            time.sleep(poll_interval)
        detail = f": {last_error}" if last_error else ""
        raise DeploymentError(f"timed out waiting for a fresh installer challenge{detail}")

    def poll_result(
        self,
        job_id: str,
        *,
        timeout: float = 180.0,
        initial_interval: float = 0.25,
        max_interval: float = 2.0,
    ) -> Result:
        validate_job_id(job_id)
        remote = f"{DEFAULT_REMOTE_ROOT}/results/{job_id}.result"
        deadline = time.monotonic() + timeout
        interval = initial_interval
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                result = parse_result(self.read_bytes(remote, max_size=4096))
                if result.job != job_id:
                    raise ProtocolError("result job id does not match the requested job")
                if result.terminal:
                    return result
            except (DeploymentError, ProtocolError) as exc:
                last_error = exc
            time.sleep(interval)
            interval = min(max_interval, interval * 1.5)
        detail = f": {last_error}" if last_error else ""
        raise DeploymentError(f"timed out waiting for deployment result{detail}")

    def read_result(self, job_id: str) -> Result | None:
        """Read one durable result without polling, or return ``None`` if absent."""

        validate_job_id(job_id)
        remote = f"{DEFAULT_REMOTE_ROOT}/results/{job_id}.result"
        data = self.read_optional_bytes(remote, max_size=4096)
        if data is None:
            return None
        result = parse_result(data)
        if result.job != job_id:
            raise ProtocolError("result job id does not match the requested job")
        return result

    def read_optional_bytes(self, remote_path: str, *, max_size: int) -> bytes | None:
        """Read an optional regular file within the deployment root."""

        self._validate_remote_path(remote_path)
        if max_size <= 0:
            raise ValueError("optional remote-file limit must be positive")
        chunks: list[bytes] = []
        size = 0

        def collect(block: bytes) -> None:
            nonlocal size
            size += len(block)
            if size > max_size:
                raise DeploymentError(f"remote file exceeds {max_size} bytes: {remote_path}")
            chunks.append(block)

        try:
            self.ftp.retrbinary(f"RETR {remote_path}", collect)
        except DeploymentError:
            raise
        except ftplib.error_perm as exc:
            response = str(exc).strip().lower()
            missing_markers = ("missing", "not found", "no such file", "does not exist")
            if (
                size == 0
                and response.startswith("550")
                and any(marker in response for marker in missing_markers)
            ):
                return None
            raise DeploymentError(f"could not read optional file {remote_path}: {exc}") from exc
        except (OSError, ftplib.Error) as exc:
            raise DeploymentError(f"could not read optional file {remote_path}: {exc}") from exc
        return b"".join(chunks)
