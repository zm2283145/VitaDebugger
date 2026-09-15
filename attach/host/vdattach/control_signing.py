"""Canonical signing transcripts for the future attach control protocol.

This module only encodes authentication inputs. It has no transport or target
mutation API, and protocol-v1 discovery remains read-only.
"""

from __future__ import annotations

from dataclasses import dataclass
from struct import pack

from .errors import ProtocolError
from .protocol import validate_title_id

CONTROL_VERSION = 1
FIXED_DEBUGGER_SLOT = 0x56444431
OPERATION_ATTACH = 1
OPERATION_DETACH = 2
OPERATION_RECOVER = 3
MIN_LEASE_MS = 250
MAX_LEASE_MS = 60_000

PEER_AUTH_DOMAIN = b"VITADEBUG-ATTACH/PEER-AUTH/v1"
OPERATION_AUTH_DOMAIN = b"VITADEBUG-ATTACH/OPERATION-AUTH/v1"
PEER_SIGNED_BYTES = 153
OPERATION_SIGNED_BYTES = 243


def _u32(value: int, name: str, *, allow_zero: bool = True) -> bytes:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > 0xFFFFFFFF
        or (not allow_zero and value == 0)
    ):
        raise ProtocolError(f"{name} must be an unsigned 32-bit integer")
    return pack(">I", value)


def _u64(value: int, name: str, *, allow_zero: bool = True) -> bytes:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > 0xFFFFFFFFFFFFFFFF
        or (not allow_zero and value == 0)
    ):
        raise ProtocolError(f"{name} must be an unsigned 64-bit integer")
    return pack(">Q", value)


def _fixed_bytes(value: bytes, size: int, name: str, *, nonzero: bool) -> bytes:
    if not isinstance(value, bytes) or len(value) != size:
        raise ProtocolError(f"{name} must be exactly {size} bytes")
    if nonzero and not any(value):
        raise ProtocolError(f"{name} must not be all zero")
    return value


@dataclass(frozen=True)
class TargetIdentity:
    title_id: str
    pid: int
    main_modid: int
    main_fingerprint: int
    target_generation: int

    def encoded(self) -> bytes:
        title = validate_title_id(self.title_id).encode("ascii")
        pid = _u32(self.pid, "PID", allow_zero=False)
        main_modid = _u32(
            self.main_modid, "main module ID", allow_zero=False
        )
        if self.pid > 0x7FFFFFFF or self.main_modid > 0x7FFFFFFF:
            raise ProtocolError("PID and main module ID must be positive SceUIDs")
        return b"".join(
            (
                title,
                pid,
                main_modid,
                _u32(
                    self.main_fingerprint,
                    "main module fingerprint",
                    allow_zero=False,
                ),
                _u64(
                    self.target_generation,
                    "target generation",
                    allow_zero=False,
                ),
            )
        )


@dataclass(frozen=True)
class PeerTranscript:
    service_generation: int
    session_id: int
    transport_binding: int
    host_key_id: int
    server_time_ms: int
    challenge_expires_at_ms: int
    expires_at_ms: int
    server_nonce: bytes
    client_nonce: bytes
    signature: bytes = bytes(64)
    version: int = CONTROL_VERSION


@dataclass(frozen=True)
class OperationAuthorization:
    operation: int
    service_generation: int
    session_id: int
    transport_binding: int
    host_key_id: int
    expires_at_ms: int
    session_expires_at_ms: int
    requested_lease_ms: int
    lease_id: int
    lease_expires_at_ms: int
    injected_module_uid: int
    target: TargetIdentity
    server_nonce: bytes
    client_nonce: bytes
    request_nonce: bytes
    signature: bytes = bytes(64)
    version: int = CONTROL_VERSION
    fixed_module_slot: int = FIXED_DEBUGGER_SLOT


def encode_peer_transcript(transcript: PeerTranscript) -> bytes:
    if not isinstance(transcript, PeerTranscript):
        raise ProtocolError("peer transcript has the wrong type")
    if transcript.version != CONTROL_VERSION:
        raise ProtocolError("peer transcript version is unsupported")
    if not (
        transcript.server_time_ms < transcript.challenge_expires_at_ms
        and transcript.server_time_ms < transcript.expires_at_ms
    ):
        raise ProtocolError("peer transcript expiration is invalid")
    # Validate but deliberately do not append the signature.
    _fixed_bytes(transcript.signature, 64, "peer signature", nonzero=False)
    result = b"".join(
        (
            PEER_AUTH_DOMAIN,
            _u32(transcript.version, "version", allow_zero=False),
            _u64(
                transcript.service_generation,
                "service generation",
                allow_zero=False,
            ),
            _u64(transcript.session_id, "session ID", allow_zero=False),
            _u64(
                transcript.transport_binding,
                "transport binding",
                allow_zero=False,
            ),
            _u64(transcript.host_key_id, "host key ID", allow_zero=False),
            _u64(transcript.server_time_ms, "server time"),
            _u64(
                transcript.challenge_expires_at_ms,
                "challenge expiration",
                allow_zero=False,
            ),
            _u64(
                transcript.expires_at_ms,
                "authorization expiration",
                allow_zero=False,
            ),
            _fixed_bytes(
                transcript.server_nonce, 32, "server nonce", nonzero=True
            ),
            _fixed_bytes(
                transcript.client_nonce, 32, "client nonce", nonzero=True
            ),
        )
    )
    if len(result) != PEER_SIGNED_BYTES:
        raise AssertionError("peer signing encoder length drifted")
    return result


def encode_operation_authorization(
    authorization: OperationAuthorization,
) -> bytes:
    if not isinstance(authorization, OperationAuthorization):
        raise ProtocolError("operation authorization has the wrong type")
    if authorization.version != CONTROL_VERSION:
        raise ProtocolError("operation authorization version is unsupported")
    if authorization.operation not in {
        OPERATION_ATTACH,
        OPERATION_DETACH,
        OPERATION_RECOVER,
    }:
        raise ProtocolError("operation is unsupported")
    if authorization.fixed_module_slot != FIXED_DEBUGGER_SLOT:
        raise ProtocolError("debugger module slot is not allowlisted")
    if (
        authorization.session_expires_at_ms == 0
        or authorization.expires_at_ms > authorization.session_expires_at_ms
    ):
        raise ProtocolError("operation expiration exceeds its session")
    if authorization.operation == OPERATION_ATTACH:
        if not MIN_LEASE_MS <= authorization.requested_lease_ms <= MAX_LEASE_MS:
            raise ProtocolError("attach lease is outside the fixed bounds")
        if any(
            (
                authorization.lease_id,
                authorization.lease_expires_at_ms,
                authorization.injected_module_uid,
            )
        ):
            raise ProtocolError("attach authorization must not claim a lease")
    elif (
        authorization.requested_lease_ms != 0
        or authorization.lease_id == 0
        or authorization.lease_expires_at_ms == 0
        or not 1 <= authorization.injected_module_uid <= 0x7FFFFFFF
    ):
        raise ProtocolError("cleanup authorization requires its owned lease")

    # Validate but deliberately do not append the signature.
    _fixed_bytes(
        authorization.signature, 64, "operation signature", nonzero=False
    )
    result = b"".join(
        (
            OPERATION_AUTH_DOMAIN,
            _u32(authorization.version, "version", allow_zero=False),
            _u32(authorization.operation, "operation", allow_zero=False),
            _u32(
                authorization.fixed_module_slot,
                "fixed debugger module slot",
                allow_zero=False,
            ),
            _u64(
                authorization.service_generation,
                "service generation",
                allow_zero=False,
            ),
            _u64(authorization.session_id, "session ID", allow_zero=False),
            _u64(
                authorization.transport_binding,
                "transport binding",
                allow_zero=False,
            ),
            _u64(
                authorization.host_key_id, "host key ID", allow_zero=False
            ),
            _u64(
                authorization.expires_at_ms,
                "authorization expiration",
                allow_zero=False,
            ),
            _u64(
                authorization.session_expires_at_ms,
                "authenticated-session expiration",
                allow_zero=False,
            ),
            _u32(authorization.requested_lease_ms, "requested lease"),
            _u64(authorization.lease_id, "lease ID"),
            _u64(authorization.lease_expires_at_ms, "lease expiration"),
            _u32(
                authorization.injected_module_uid, "injected module UID"
            ),
            authorization.target.encoded(),
            _fixed_bytes(
                authorization.server_nonce, 32, "server nonce", nonzero=True
            ),
            _fixed_bytes(
                authorization.client_nonce, 32, "client nonce", nonzero=True
            ),
            _fixed_bytes(
                authorization.request_nonce, 32, "request nonce", nonzero=True
            ),
        )
    )
    if len(result) != OPERATION_SIGNED_BYTES:
        raise AssertionError("operation signing encoder length drifted")
    return result
