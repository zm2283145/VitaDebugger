"""Authenticated direct-TCP staging for VitaDevDeploy.

The TCP transport does not replace the signed job format.  It carries the
same request, manifest, signature, and manifest-ordered package bytes that the
FTP transport writes under ``ux0:data/VitaDevDeploy/inbox``.  The Vita must
authenticate the metadata before replying with the first acknowledgement and
must commit ``request.v1`` only after every package byte is durable.
"""

from __future__ import annotations

import socket
import struct
import time
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable

from .errors import DeploymentError, ProtocolError
from .job import PreparedJob, load_prepared_job
from .manifest import Manifest, verify_manifest
from .paths import normalize_package_path, validate_job_id
from .protocol import Challenge


DEFAULT_DIRECT_PORT = 18196

GREETING_MAGIC = b"VDDCHL01"
JOB_MAGIC = b"VDDJOB01"
ACK_MAGIC = b"VDDACK01"

ACK_AUTHENTICATED = 1
ACK_COMMITTED = 2
VDEV_ERR_REPLAY = -20012
VDEV_ERR_COMMIT_UNKNOWN = -20021

_GREETING = struct.Struct("!8s32s")
_JOB_HEADER = struct.Struct("!8sIIIQ")
_ACK = struct.Struct("!8sIiIQ")


class DirectCommitState(str, Enum):
    """What the PC can prove about the on-device request commit point."""

    FAIL_BEFORE_COMMIT = "fail_before_commit"
    AMBIGUOUS_AFTER_COMMIT = "ambiguous_after_commit"
    KNOWN_COMMITTED = "known_committed"


class DirectTransferError(DeploymentError):
    """A direct-transfer failure carrying its conservative commit state."""

    def __init__(
        self,
        message: str,
        *,
        commit_state: DirectCommitState,
        job: str | None = None,
        result_may_appear: bool = False,
    ) -> None:
        super().__init__(message)
        self.commit_state = commit_state
        self.job = job
        # Cleanup uncertainty before phase 1 cannot produce an install result;
        # post-phase-1 uncertainty can.  Keep that distinction separate from
        # the conservative commit-state label so callers do not poll for a
        # result which cannot exist.
        self.result_may_appear = result_may_appear


@dataclass(frozen=True)
class DirectIntakeReceipt:
    """A protocol-v1 receiver's unauthenticated commit report."""

    job: str
    resumed_files: int
    resumed_bytes: int
    committed_files: int
    committed_bytes: int
    commit_state: DirectCommitState = DirectCommitState.AMBIGUOUS_AFTER_COMMIT


@dataclass(frozen=True)
class DirectTransferPlan:
    """Fully validated local metadata prepared before a receiver deadline."""

    job: PreparedJob
    manifest: Manifest
    request_data: bytes
    manifest_data: bytes
    signature: bytes


def prepare_direct_transfer(
    job_or_path: PreparedJob | Path | str,
) -> DirectTransferPlan:
    """Validate and hash a signed job without requiring a TCP connection."""

    # Reload even a PreparedJob so the plan is derived from the exact bytes
    # which will be sent, rather than from request/manifest objects cached
    # before a caller-visible filesystem change.
    source = job_or_path.root if isinstance(job_or_path, PreparedJob) else job_or_path
    job = load_prepared_job(source)
    validate_job_id(job.request.job)
    manifest_data = job.manifest_path.read_bytes()
    manifest = verify_manifest(job.package, manifest_data)
    request_data = job.request_path.read_bytes()
    signature = job.signature_path.read_bytes()
    if len(signature) != 64:
        raise ProtocolError("signature.bin must contain exactly 64 raw bytes")
    if (
        manifest.file_count != job.request.file_count
        or manifest.total_size != job.request.total_size
    ):
        raise ProtocolError("prepared job manifest counts changed before direct transfer")
    for entry in manifest.entries:
        normalized = normalize_package_path(entry.path)
        if normalized.value != entry.path:
            raise ProtocolError("manifest contains a noncanonical path")
    return DirectTransferPlan(
        job=job,
        manifest=manifest,
        request_data=request_data,
        manifest_data=manifest_data,
        signature=signature,
    )


def encode_greeting(nonce: str) -> bytes:
    """Encode the fixed-size Vita greeting used by host fakes and fixtures."""

    try:
        raw = bytes.fromhex(nonce)
    except ValueError as exc:
        raise ProtocolError("direct greeting nonce is not lowercase hexadecimal") from exc
    if len(raw) != 32 or nonce != raw.hex():
        raise ProtocolError("direct greeting nonce must be 64 lowercase hexadecimal characters")
    return _GREETING.pack(GREETING_MAGIC, raw)


def decode_greeting(data: bytes) -> Challenge:
    if len(data) != _GREETING.size:
        raise ProtocolError("direct greeting has the wrong size")
    magic, nonce = _GREETING.unpack(data)
    if magic != GREETING_MAGIC:
        raise ProtocolError("direct greeting magic is invalid")
    return Challenge(nonce.hex())


def encode_ack(phase: int, code: int, file_count: int, byte_count: int) -> bytes:
    """Encode a receiver acknowledgement for deterministic host-side tests."""

    if phase not in {ACK_AUTHENTICATED, ACK_COMMITTED}:
        raise ValueError("unknown direct acknowledgement phase")
    if not -(2**31) <= code <= 2**31 - 1:
        raise ValueError("direct acknowledgement code is outside int32 range")
    if not 0 <= file_count <= 2**32 - 1 or not 0 <= byte_count <= 2**64 - 1:
        raise ValueError("direct acknowledgement progress is outside wire range")
    return _ACK.pack(ACK_MAGIC, phase, code, file_count, byte_count)


def decode_job_header(data: bytes) -> tuple[int, int, int, int]:
    """Decode the fixed metadata header; intended for receivers and tests."""

    if len(data) != _JOB_HEADER.size:
        raise ProtocolError("direct job header has the wrong size")
    magic, request_size, manifest_size, signature_size, package_size = _JOB_HEADER.unpack(data)
    if magic != JOB_MAGIC:
        raise ProtocolError("direct job header magic is invalid")
    if not 0 < request_size <= 4096:
        raise ProtocolError("direct request size is outside the protocol limit")
    if not 0 < manifest_size <= 4 * 1024 * 1024:
        raise ProtocolError("direct manifest size is outside the protocol limit")
    if signature_size != 64:
        raise ProtocolError("direct signature size must be exactly 64 bytes")
    if package_size > 1024 * 1024 * 1024:
        raise ProtocolError("direct package size is outside the protocol limit")
    return request_size, manifest_size, signature_size, package_size


class VitaDirectClient:
    """One-connection direct staging client.

    The receiver may resume only at a complete manifest-entry boundary.  The
    acknowledgement's byte count must exactly equal the sizes of the reported
    prefix, preventing either peer from interpreting an arbitrary byte offset.
    """

    def __init__(
        self,
        host: str,
        *,
        port: int = DEFAULT_DIRECT_PORT,
        timeout: float = 10.0,
        socket_factory: Callable[..., socket.socket] = socket.create_connection,
    ) -> None:
        if not 1 <= port <= 65535:
            raise ValueError("direct TCP port must be between 1 and 65535")
        if timeout <= 0:
            raise ValueError("direct TCP timeout must be positive")
        self.host = host
        self.port = port
        self.timeout = timeout
        self._socket_factory = socket_factory
        self._socket: socket.socket | None = None
        self.challenge: Challenge | None = None

    def connect(self) -> "VitaDirectClient":
        self.close()
        try:
            connection = self._socket_factory((self.host, self.port), timeout=self.timeout)
            connection.settimeout(self.timeout)
            self._socket = connection
            self.challenge = decode_greeting(self._recv_exact(_GREETING.size))
            return self
        except (OSError, ProtocolError, DeploymentError) as exc:
            self.close()
            if isinstance(exc, DeploymentError):
                raise
            raise DeploymentError(f"could not connect to VitaDevDeploy direct TCP intake: {exc}") from exc

    def connect_wait(self, *, timeout: float, poll_interval: float = 0.25) -> "VitaDirectClient":
        if timeout <= 0 or poll_interval <= 0:
            raise ValueError("direct connection wait and interval must be positive")
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                return self.connect()
            except DeploymentError as exc:
                last_error = exc
                remaining = deadline - time.monotonic()
                if remaining > 0:
                    time.sleep(min(poll_interval, remaining))
        detail = f": {last_error}" if last_error else ""
        raise DeploymentError(f"timed out waiting for VitaDevDeploy direct TCP intake{detail}")

    def close(self) -> None:
        connection, self._socket = self._socket, None
        self.challenge = None
        if connection is not None:
            try:
                connection.close()
            except OSError:
                pass

    def __enter__(self) -> "VitaDirectClient":
        return self.connect()

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    @property
    def connection(self) -> socket.socket:
        if self._socket is None:
            raise DeploymentError("direct TCP client is not connected")
        return self._socket

    def _recv_exact(self, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            try:
                block = self.connection.recv(size - len(data))
            except OSError as exc:
                raise DeploymentError(f"direct TCP receive failed: {exc}") from exc
            if not block:
                raise DeploymentError("direct TCP peer closed before completing a protocol frame")
            data.extend(block)
        return bytes(data)

    def _send(self, data: bytes | memoryview) -> None:
        try:
            self.connection.sendall(data)
        except OSError as exc:
            raise DeploymentError(f"direct TCP send failed: {exc}") from exc

    def _read_ack(self, expected_phase: int) -> tuple[int, int, int]:
        magic, phase, code, file_count, byte_count = _ACK.unpack(self._recv_exact(_ACK.size))
        if magic != ACK_MAGIC or phase != expected_phase:
            raise ProtocolError("direct receiver returned an invalid acknowledgement")
        return code, file_count, byte_count

    def stage_job(self, plan: DirectTransferPlan) -> DirectIntakeReceipt:
        if not isinstance(plan, DirectTransferPlan):
            raise TypeError(
                "stage_job requires an offline DirectTransferPlan from "
                "prepare_direct_transfer()"
            )
        job = plan.job
        payload_authorized = False
        try:
            if self.challenge is None:
                raise ProtocolError("direct TCP greeting was not received")
            if job.request.nonce != self.challenge.nonce:
                raise ProtocolError("prepared job nonce does not match the direct TCP greeting")

            manifest = plan.manifest
            request_data = plan.request_data
            manifest_data = plan.manifest_data
            signature = plan.signature

            self._send(
                _JOB_HEADER.pack(
                    JOB_MAGIC,
                    len(request_data),
                    len(manifest_data),
                    len(signature),
                    manifest.total_size,
                )
            )
            self._send(request_data)
            self._send(manifest_data)
            self._send(signature)

            code, resume_files, resume_bytes = self._read_ack(ACK_AUTHENTICATED)
            if code != 0:
                if code == VDEV_ERR_REPLAY:
                    # A replay response means this job ID already has either a
                    # committed request or a durable result. Protocol-v1 ACKs
                    # are not authenticated, so retain the signed handle and
                    # reconcile through the result channel instead of treating
                    # the negative frame as deletion evidence.
                    raise DirectTransferError(
                        "VitaDevDeploy reports that this signed job may already be committed",
                        commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                        job=job.request.job,
                        result_may_appear=True,
                    )
                if code == VDEV_ERR_COMMIT_UNKNOWN:
                    raise DirectTransferError(
                        "VitaDevDeploy could not prove cleanup of authenticated metadata",
                        commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                        job=job.request.job,
                    )
                raise DirectTransferError(
                    f"VitaDevDeploy direct receiver rejected metadata with code {code}",
                    commit_state=DirectCommitState.FAIL_BEFORE_COMMIT,
                    job=job.request.job,
                )
            # A positive phase-1 report says the receiver authenticated the
            # metadata, but ACK01 itself is unauthenticated. Even malformed
            # resume progress from this point is not proof that request.v1
            # stayed uncommitted, so preserve ambiguity through reconciliation.
            payload_authorized = True
            if resume_files > manifest.file_count:
                raise ProtocolError("direct receiver resume file count exceeds the manifest")
            expected_resume_bytes = sum(entry.size for entry in manifest.entries[:resume_files])
            if resume_bytes != expected_resume_bytes:
                raise ProtocolError("direct receiver resume byte count is not a manifest boundary")

            # From this point until a valid phase-2 response, a transport error
            # cannot prove whether the Vita received the final byte and renamed
            # request.v1. Preserve that uncertainty instead of reporting a
            # definite pre-commit failure.
            for entry in manifest.entries[resume_files:]:
                path = job.package.joinpath(*entry.path.split("/"))
                try:
                    with path.open("rb") as stream:
                        remaining = entry.size
                        while remaining:
                            block = stream.read(min(1024 * 1024, remaining))
                            if not block:
                                raise ProtocolError(
                                    f"package file became shorter during direct transfer: {entry.path}"
                                )
                            self._send(block)
                            remaining -= len(block)
                        if stream.read(1):
                            raise ProtocolError(
                                f"package file became longer during direct transfer: {entry.path}"
                            )
                except OSError as exc:
                    raise DeploymentError(
                        f"could not stream package file {entry.path}: {exc}"
                    ) from exc

            code, committed_files, committed_bytes = self._read_ack(ACK_COMMITTED)
            if code in {VDEV_ERR_COMMIT_UNKNOWN, VDEV_ERR_REPLAY}:
                detail = (
                    "reports that this signed job may already be committed"
                    if code == VDEV_ERR_REPLAY
                    else "could not prove whether request.v1 remained committed"
                )
                raise DirectTransferError(
                    f"VitaDevDeploy {detail}",
                    commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                    job=job.request.job,
                    result_may_appear=True,
                )
            if code != 0:
                # ACK01 has no peer-authentication or job/nonce binding. Once
                # payload transmission was authorized, a negative frame cannot
                # prove that the genuine receiver stayed before its commit
                # point. Preserve the job and reconcile a possible result.
                raise DirectTransferError(
                    f"VitaDevDeploy returned an unauthenticated negative payload acknowledgement with code {code}",
                    commit_state=DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                    job=job.request.job,
                    result_may_appear=True,
                )
            if (
                committed_files != manifest.file_count
                or committed_bytes != manifest.total_size
            ):
                raise ProtocolError(
                    "direct receiver committed progress does not match the signed manifest"
                )
            return DirectIntakeReceipt(
                job=job.request.job,
                resumed_files=resume_files,
                resumed_bytes=resume_bytes,
                committed_files=committed_files,
                committed_bytes=committed_bytes,
            )
        except DirectTransferError:
            raise
        except (DeploymentError, ProtocolError, OSError) as exc:
            state = (
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT
                if payload_authorized
                else DirectCommitState.FAIL_BEFORE_COMMIT
            )
            raise DirectTransferError(
                str(exc),
                commit_state=state,
                job=job.request.job,
                result_may_appear=payload_authorized,
            ) from exc


__all__ = [
    "ACK_AUTHENTICATED",
    "ACK_COMMITTED",
    "ACK_MAGIC",
    "DEFAULT_DIRECT_PORT",
    "DirectCommitState",
    "DirectIntakeReceipt",
    "DirectTransferPlan",
    "DirectTransferError",
    "GREETING_MAGIC",
    "JOB_MAGIC",
    "VitaDirectClient",
    "VDEV_ERR_COMMIT_UNKNOWN",
    "VDEV_ERR_REPLAY",
    "decode_greeting",
    "decode_job_header",
    "encode_ack",
    "encode_greeting",
    "prepare_direct_transfer",
]
