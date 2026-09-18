"""Canonical authenticated attach handshake protocol version 2.

Version 2 currently defines only mutual authentication and session creation.
It has no discovery, process, module, memory, or debugger operation record.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, replace

from .errors import ProtocolError


WIRE_MAGIC = b"VDA2"
WIRE_VERSION = 2
MAX_AUTH_FRAME_SIZE = 1024
NONCE_BYTES = 32
SIGNATURE_BYTES = 64

TYPE_HELLO = 1
TYPE_CHALLENGE = 2
TYPE_PROOF = 3
TYPE_RESULT = 4

STATUS_OK = 0
STATUS_DENIED = 1
STATUS_RATE_LIMITED = 2
STATUS_SHUTDOWN = 3
KNOWN_STATUSES = frozenset(
    {STATUS_OK, STATUS_DENIED, STATUS_RATE_LIMITED, STATUS_SHUTDOWN}
)

SERVER_CHALLENGE_DOMAIN = b"VITADEBUG-ATTACH/SERVER-CHALLENGE/v2"
CLIENT_PROOF_DOMAIN = b"VITADEBUG-ATTACH/CLIENT-PROOF/v2"
SESSION_RESULT_DOMAIN = b"VITADEBUG-ATTACH/SESSION-RESULT/v2"

_HEADER = struct.Struct(">4sBBH")
_HELLO = struct.Struct(">QQ32s32s")
_CHALLENGE = struct.Struct(">9Q32s32s32s64s")
_PROOF = struct.Struct(">10Q32s32s32s64s")
_RESULT = struct.Struct(">10QII32s32s32s32s64s")


def _u64(value: int, label: str, *, nonzero: bool = True) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > 0xFFFFFFFFFFFFFFFF
        or (nonzero and value == 0)
    ):
        suffix = "nonzero " if nonzero else ""
        raise ProtocolError(
            f"{label} must be a {suffix}unsigned 64-bit integer"
        )
    return value


def _u32(value: int, label: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > 0xFFFFFFFF
    ):
        raise ProtocolError(f"{label} must be an unsigned 32-bit integer")
    return value


def _fixed(
    value: bytes, size: int, label: str, *, nonzero: bool
) -> bytes:
    if not isinstance(value, bytes) or len(value) != size:
        raise ProtocolError(f"{label} must be exactly {size} bytes")
    if nonzero and not any(value):
        raise ProtocolError(f"{label} must not be all zero")
    return value


def _payload(record_type: int, body: bytes) -> bytes:
    if not 1 <= record_type <= 0xFF:
        raise ProtocolError("authenticated record type is invalid")
    size = _HEADER.size + len(body)
    if size > MAX_AUTH_FRAME_SIZE:
        raise ProtocolError("authenticated record exceeds its frame limit")
    return _HEADER.pack(
        WIRE_MAGIC, WIRE_VERSION, record_type, len(body)
    ) + body


def _body(data: bytes, expected_type: int, layout: struct.Struct) -> tuple:
    if not isinstance(data, bytes):
        raise ProtocolError("authenticated record must be bytes")
    if len(data) != _HEADER.size + layout.size:
        raise ProtocolError("authenticated record has a noncanonical size")
    magic, version, record_type, body_size = _HEADER.unpack_from(data)
    if magic != WIRE_MAGIC or version != WIRE_VERSION:
        raise ProtocolError("authenticated record version is unsupported")
    if record_type != expected_type or body_size != layout.size:
        raise ProtocolError("authenticated record type or body size is invalid")
    return layout.unpack_from(data, _HEADER.size)


def frame_record(data: bytes) -> bytes:
    if (
        not isinstance(data, bytes)
        or not 1 <= len(data) <= MAX_AUTH_FRAME_SIZE
    ):
        raise ProtocolError(
            f"authenticated frame must contain 1 through "
            f"{MAX_AUTH_FRAME_SIZE} bytes"
        )
    return struct.pack(">I", len(data)) + data


def parse_frame_prefix(prefix: bytes) -> int:
    if not isinstance(prefix, bytes) or len(prefix) != 4:
        raise ProtocolError("authenticated frame prefix must be four bytes")
    size = struct.unpack(">I", prefix)[0]
    if not 1 <= size <= MAX_AUTH_FRAME_SIZE:
        raise ProtocolError(
            f"authenticated frame length must be from 1 through "
            f"{MAX_AUTH_FRAME_SIZE}"
        )
    return size


@dataclass(frozen=True)
class AuthHello:
    host_key_id: int
    host_key_generation: int
    request_nonce: bytes
    client_nonce: bytes

    def encode(self) -> bytes:
        return _payload(
            TYPE_HELLO,
            _HELLO.pack(
                _u64(self.host_key_id, "host key ID"),
                _u64(self.host_key_generation, "host key generation"),
                _fixed(
                    self.request_nonce,
                    NONCE_BYTES,
                    "request nonce",
                    nonzero=True,
                ),
                _fixed(
                    self.client_nonce,
                    NONCE_BYTES,
                    "client nonce",
                    nonzero=True,
                ),
            ),
        )

    @classmethod
    def decode(cls, data: bytes) -> "AuthHello":
        result = cls(*_body(data, TYPE_HELLO, _HELLO))
        result.encode()
        return result


@dataclass(frozen=True)
class AuthChallenge:
    host_key_id: int
    host_key_generation: int
    server_key_id: int
    server_key_generation: int
    service_generation: int
    session_id: int
    transport_binding: int
    server_time_ms: int
    challenge_expires_at_ms: int
    request_nonce: bytes
    client_nonce: bytes
    server_nonce: bytes
    signature: bytes = bytes(SIGNATURE_BYTES)

    def _values(self) -> tuple:
        if self.server_time_ms >= self.challenge_expires_at_ms:
            raise ProtocolError("challenge expiration is invalid")
        return (
            _u64(self.host_key_id, "host key ID"),
            _u64(self.host_key_generation, "host key generation"),
            _u64(self.server_key_id, "server key ID"),
            _u64(self.server_key_generation, "server key generation"),
            _u64(self.service_generation, "service generation"),
            _u64(self.session_id, "session ID"),
            _u64(self.transport_binding, "transport binding"),
            _u64(self.server_time_ms, "server time", nonzero=False),
            _u64(
                self.challenge_expires_at_ms,
                "challenge expiration",
            ),
            _fixed(
                self.request_nonce,
                NONCE_BYTES,
                "request nonce",
                nonzero=True,
            ),
            _fixed(
                self.client_nonce,
                NONCE_BYTES,
                "client nonce",
                nonzero=True,
            ),
            _fixed(
                self.server_nonce,
                NONCE_BYTES,
                "server nonce",
                nonzero=True,
            ),
        )

    def transcript(self) -> bytes:
        return SERVER_CHALLENGE_DOMAIN + _CHALLENGE.pack(
            *self._values(), bytes(SIGNATURE_BYTES)
        )[:-SIGNATURE_BYTES]

    def encode(self) -> bytes:
        return _payload(
            TYPE_CHALLENGE,
            _CHALLENGE.pack(
                *self._values(),
                _fixed(
                    self.signature,
                    SIGNATURE_BYTES,
                    "server signature",
                    nonzero=True,
                ),
            ),
        )

    @classmethod
    def decode(cls, data: bytes) -> "AuthChallenge":
        result = cls(*_body(data, TYPE_CHALLENGE, _CHALLENGE))
        result.encode()
        return result

    def with_signature(self, signature: bytes) -> "AuthChallenge":
        return replace(self, signature=signature)


@dataclass(frozen=True)
class AuthProof:
    host_key_id: int
    host_key_generation: int
    server_key_id: int
    server_key_generation: int
    service_generation: int
    session_id: int
    transport_binding: int
    server_time_ms: int
    challenge_expires_at_ms: int
    proof_expires_at_ms: int
    request_nonce: bytes
    client_nonce: bytes
    server_nonce: bytes
    signature: bytes = bytes(SIGNATURE_BYTES)

    @classmethod
    def from_challenge(
        cls, challenge: AuthChallenge, proof_expires_at_ms: int
    ) -> "AuthProof":
        return cls(
            challenge.host_key_id,
            challenge.host_key_generation,
            challenge.server_key_id,
            challenge.server_key_generation,
            challenge.service_generation,
            challenge.session_id,
            challenge.transport_binding,
            challenge.server_time_ms,
            challenge.challenge_expires_at_ms,
            proof_expires_at_ms,
            challenge.request_nonce,
            challenge.client_nonce,
            challenge.server_nonce,
        )

    def _values(self) -> tuple:
        if (
            self.server_time_ms >= self.challenge_expires_at_ms
            or self.server_time_ms >= self.proof_expires_at_ms
            or self.proof_expires_at_ms > self.challenge_expires_at_ms
        ):
            raise ProtocolError("client proof expiration is invalid")
        return (
            _u64(self.host_key_id, "host key ID"),
            _u64(self.host_key_generation, "host key generation"),
            _u64(self.server_key_id, "server key ID"),
            _u64(self.server_key_generation, "server key generation"),
            _u64(self.service_generation, "service generation"),
            _u64(self.session_id, "session ID"),
            _u64(self.transport_binding, "transport binding"),
            _u64(self.server_time_ms, "server time", nonzero=False),
            _u64(
                self.challenge_expires_at_ms,
                "challenge expiration",
            ),
            _u64(self.proof_expires_at_ms, "proof expiration"),
            _fixed(
                self.request_nonce,
                NONCE_BYTES,
                "request nonce",
                nonzero=True,
            ),
            _fixed(
                self.client_nonce,
                NONCE_BYTES,
                "client nonce",
                nonzero=True,
            ),
            _fixed(
                self.server_nonce,
                NONCE_BYTES,
                "server nonce",
                nonzero=True,
            ),
        )

    def transcript(self) -> bytes:
        return CLIENT_PROOF_DOMAIN + _PROOF.pack(
            *self._values(), bytes(SIGNATURE_BYTES)
        )[:-SIGNATURE_BYTES]

    def encode(self) -> bytes:
        return _payload(
            TYPE_PROOF,
            _PROOF.pack(
                *self._values(),
                _fixed(
                    self.signature,
                    SIGNATURE_BYTES,
                    "host signature",
                    nonzero=True,
                ),
            ),
        )

    @classmethod
    def decode(cls, data: bytes) -> "AuthProof":
        result = cls(*_body(data, TYPE_PROOF, _PROOF))
        result.encode()
        return result

    def with_signature(self, signature: bytes) -> "AuthProof":
        return replace(self, signature=signature)


@dataclass(frozen=True)
class AuthResult:
    host_key_id: int
    host_key_generation: int
    server_key_id: int
    server_key_generation: int
    service_generation: int
    session_id: int
    transport_binding: int
    server_time_ms: int
    challenge_expires_at_ms: int
    proof_expires_at_ms: int
    status: int
    retry_after_ms: int
    request_nonce: bytes
    client_nonce: bytes
    server_nonce: bytes
    session_nonce: bytes
    signature: bytes = bytes(SIGNATURE_BYTES)

    @classmethod
    def from_proof(
        cls,
        proof: AuthProof,
        *,
        status: int,
        retry_after_ms: int,
        session_nonce: bytes,
    ) -> "AuthResult":
        return cls(
            proof.host_key_id,
            proof.host_key_generation,
            proof.server_key_id,
            proof.server_key_generation,
            proof.service_generation,
            proof.session_id,
            proof.transport_binding,
            proof.server_time_ms,
            proof.challenge_expires_at_ms,
            proof.proof_expires_at_ms,
            status,
            retry_after_ms,
            proof.request_nonce,
            proof.client_nonce,
            proof.server_nonce,
            session_nonce,
        )

    def _values(self) -> tuple:
        if self.status not in KNOWN_STATUSES:
            raise ProtocolError("authentication result status is invalid")
        if (
            self.server_time_ms >= self.challenge_expires_at_ms
            or self.server_time_ms >= self.proof_expires_at_ms
            or self.proof_expires_at_ms > self.challenge_expires_at_ms
        ):
            raise ProtocolError("authentication result expiration is invalid")
        retry = _u32(self.retry_after_ms, "retry delay")
        session_nonce = _fixed(
            self.session_nonce,
            NONCE_BYTES,
            "session nonce",
            nonzero=self.status == STATUS_OK,
        )
        if self.status == STATUS_OK and retry != 0:
            raise ProtocolError("successful authentication has a retry delay")
        if self.status != STATUS_OK and any(session_nonce):
            raise ProtocolError("failed authentication cannot issue a session")
        return (
            _u64(self.host_key_id, "host key ID"),
            _u64(self.host_key_generation, "host key generation"),
            _u64(self.server_key_id, "server key ID"),
            _u64(self.server_key_generation, "server key generation"),
            _u64(self.service_generation, "service generation"),
            _u64(self.session_id, "session ID"),
            _u64(self.transport_binding, "transport binding"),
            _u64(self.server_time_ms, "server time", nonzero=False),
            _u64(
                self.challenge_expires_at_ms,
                "challenge expiration",
            ),
            _u64(self.proof_expires_at_ms, "proof expiration"),
            _u32(self.status, "authentication status"),
            retry,
            _fixed(
                self.request_nonce,
                NONCE_BYTES,
                "request nonce",
                nonzero=True,
            ),
            _fixed(
                self.client_nonce,
                NONCE_BYTES,
                "client nonce",
                nonzero=True,
            ),
            _fixed(
                self.server_nonce,
                NONCE_BYTES,
                "server nonce",
                nonzero=True,
            ),
            session_nonce,
        )

    def transcript(self) -> bytes:
        return SESSION_RESULT_DOMAIN + _RESULT.pack(
            *self._values(), bytes(SIGNATURE_BYTES)
        )[:-SIGNATURE_BYTES]

    def encode(self) -> bytes:
        return _payload(
            TYPE_RESULT,
            _RESULT.pack(
                *self._values(),
                _fixed(
                    self.signature,
                    SIGNATURE_BYTES,
                    "result signature",
                    nonzero=True,
                ),
            ),
        )

    @classmethod
    def decode(cls, data: bytes) -> "AuthResult":
        result = cls(*_body(data, TYPE_RESULT, _RESULT))
        result.encode()
        return result

    def with_signature(self, signature: bytes) -> "AuthResult":
        return replace(self, signature=signature)
