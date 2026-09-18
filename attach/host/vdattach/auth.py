"""Mutual-authentication authority and bounded host client for protocol v2."""

from __future__ import annotations

import hmac
import secrets
import socket
import threading
import time
from dataclasses import dataclass
from typing import Callable

from .auth_keys import KeyStore
from .auth_protocol import (
    MAX_AUTH_FRAME_SIZE,
    NONCE_BYTES,
    STATUS_DENIED,
    STATUS_OK,
    AuthChallenge,
    AuthHello,
    AuthProof,
    AuthResult,
    frame_record,
    parse_frame_prefix,
)
from .client import checked_timeout
from .errors import (
    AuthenticationError,
    ProtocolError,
    RateLimitError,
    TransportError,
)


MAX_OUTSTANDING_CHALLENGES = 16
MAX_REPLAY_REQUESTS = 128
MAX_AUTHENTICATED_SESSIONS = 8
MIN_CHALLENGE_MS = 100
MAX_CHALLENGE_MS = 30_000
MAX_PROOF_WINDOW_MS = 60_000
MIN_BACKOFF_MS = 250
MAX_BACKOFF_MS = 8_000


def _deadline_timeout(stream: socket.socket, deadline: float) -> None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TransportError("authenticated attach exchange timed out")
    stream.settimeout(remaining)


def _recv_exact(
    stream: socket.socket, size: int, deadline: float
) -> bytes:
    output = bytearray()
    while len(output) < size:
        _deadline_timeout(stream, deadline)
        try:
            chunk = stream.recv(size - len(output))
        except socket.timeout as exc:
            raise TransportError(
                "authenticated attach exchange timed out"
            ) from exc
        except OSError as exc:
            raise TransportError(
                f"authenticated attach receive failed: {exc}"
            ) from exc
        if not chunk:
            raise TransportError(
                "authenticated attach peer disconnected mid-frame"
            )
        output.extend(chunk)
    return bytes(output)


def receive_auth_record(stream: socket.socket, deadline: float) -> bytes:
    size = parse_frame_prefix(_recv_exact(stream, 4, deadline))
    return _recv_exact(stream, size, deadline)


def send_auth_record(
    stream: socket.socket, data: bytes, deadline: float
) -> None:
    framed = frame_record(data)
    _deadline_timeout(stream, deadline)
    try:
        stream.sendall(framed)
    except socket.timeout as exc:
        raise TransportError(
            "authenticated attach exchange timed out"
        ) from exc
    except OSError as exc:
        raise TransportError(
            f"authenticated attach send failed: {exc}"
        ) from exc


def _same_bytes(left: bytes, right: bytes) -> bool:
    return hmac.compare_digest(left, right)


def _same_u64(left: int, right: int) -> bool:
    return hmac.compare_digest(
        left.to_bytes(8, "big"), right.to_bytes(8, "big")
    )


def _challenge_matches_proof(
    challenge: AuthChallenge, proof: AuthProof
) -> bool:
    return (
        _same_u64(challenge.host_key_id, proof.host_key_id)
        and _same_u64(
            challenge.host_key_generation, proof.host_key_generation
        )
        and _same_u64(challenge.server_key_id, proof.server_key_id)
        and _same_u64(
            challenge.server_key_generation, proof.server_key_generation
        )
        and _same_u64(
            challenge.service_generation, proof.service_generation
        )
        and _same_u64(challenge.session_id, proof.session_id)
        and _same_u64(
            challenge.transport_binding, proof.transport_binding
        )
        and challenge.server_time_ms == proof.server_time_ms
        and challenge.challenge_expires_at_ms
        == proof.challenge_expires_at_ms
        and _same_bytes(challenge.request_nonce, proof.request_nonce)
        and _same_bytes(challenge.client_nonce, proof.client_nonce)
        and _same_bytes(challenge.server_nonce, proof.server_nonce)
    )


def _proof_matches_result(proof: AuthProof, result: AuthResult) -> bool:
    return (
        _same_u64(proof.host_key_id, result.host_key_id)
        and _same_u64(
            proof.host_key_generation, result.host_key_generation
        )
        and _same_u64(proof.server_key_id, result.server_key_id)
        and _same_u64(
            proof.server_key_generation, result.server_key_generation
        )
        and _same_u64(proof.service_generation, result.service_generation)
        and _same_u64(proof.session_id, result.session_id)
        and _same_u64(proof.transport_binding, result.transport_binding)
        and proof.server_time_ms == result.server_time_ms
        and proof.challenge_expires_at_ms
        == result.challenge_expires_at_ms
        and proof.proof_expires_at_ms == result.proof_expires_at_ms
        and _same_bytes(proof.request_nonce, result.request_nonce)
        and _same_bytes(proof.client_nonce, result.client_nonce)
        and _same_bytes(proof.server_nonce, result.server_nonce)
    )


@dataclass(frozen=True)
class AuthenticatedSession:
    service_generation: int
    session_id: int
    transport_binding: int
    host_key_id: int
    host_key_generation: int
    server_key_id: int
    server_key_generation: int
    session_nonce: bytes
    expires_at_ms: int


@dataclass
class _FailureState:
    failures: int = 0
    blocked_until_ms: int = 0


@dataclass(frozen=True)
class _PendingChallenge:
    challenge: AuthChallenge
    peer_binding: int


class AuthenticationAuthority:
    """Allocation-bounded Vita-side model with injected keys and clock."""

    def __init__(
        self,
        key_store: KeyStore,
        *,
        now_ms: Callable[[], int],
        entropy: Callable[[int], bytes] = secrets.token_bytes,
        challenge_timeout_ms: int = 3_000,
    ) -> None:
        if (
            isinstance(challenge_timeout_ms, bool)
            or not MIN_CHALLENGE_MS
            <= challenge_timeout_ms
            <= MAX_CHALLENGE_MS
        ):
            raise ProtocolError("challenge timeout is outside fixed bounds")
        self._keys = key_store
        self._now_ms = now_ms
        self._entropy = entropy
        self._challenge_timeout_ms = challenge_timeout_ms
        self._generation = int.from_bytes(self._nonce(8), "big")
        if self._generation == 0:
            raise ProtocolError("entropy returned a zero service generation")
        self._next_session_id = 1
        self._pending: dict[int, _PendingChallenge] = {}
        self._sessions: dict[int, AuthenticatedSession] = {}
        self._request_replay: list[bytes] = []
        self._failures: dict[tuple[int, int], _FailureState] = {}
        self._connection_lock = threading.Lock()
        self._connections: set[socket.socket] = set()
        self._shutting_down = False

    @property
    def service_generation(self) -> int:
        return self._generation

    def _nonce(self, size: int) -> bytes:
        value = self._entropy(size)
        if not isinstance(value, bytes) or len(value) != size or not any(value):
            raise ProtocolError("entropy provider failed closed")
        return value

    def _failure_key(self, peer_binding: int, host_key_id: int) -> tuple[int, int]:
        return peer_binding, host_key_id

    def _retry_after(self, peer_binding: int, host_key_id: int) -> int:
        state = self._failures.get(
            self._failure_key(peer_binding, host_key_id)
        )
        if state is None:
            return 0
        return max(0, state.blocked_until_ms - self._now_ms())

    def _record_failure(self, peer_binding: int, host_key_id: int) -> int:
        key = self._failure_key(peer_binding, host_key_id)
        state = self._failures.setdefault(key, _FailureState())
        state.failures = min(state.failures + 1, 31)
        delay = min(
            MIN_BACKOFF_MS << min(state.failures - 1, 5),
            MAX_BACKOFF_MS,
        )
        state.blocked_until_ms = self._now_ms() + delay
        return delay

    def issue_challenge(
        self, hello: AuthHello, *, peer_binding: int
    ) -> AuthChallenge:
        if self._shutting_down:
            raise AuthenticationError("authentication authority is shut down")
        if (
            isinstance(peer_binding, bool)
            or not isinstance(peer_binding, int)
            or not 1 <= peer_binding <= 0xFFFFFFFFFFFFFFFF
        ):
            raise ProtocolError("transport binding is invalid")
        hello.encode()
        retry = self._retry_after(peer_binding, hello.host_key_id)
        if retry:
            raise RateLimitError(retry)
        if len(self._pending) >= MAX_OUTSTANDING_CHALLENGES:
            raise AuthenticationError("authentication challenge limit reached")
        if any(
            _same_bytes(existing, hello.request_nonce)
            for existing in self._request_replay
        ):
            self._record_failure(peer_binding, hello.host_key_id)
            raise AuthenticationError("authentication request replay rejected")
        if len(self._request_replay) >= MAX_REPLAY_REQUESTS:
            raise AuthenticationError(
                "authentication replay table requires service restart"
            )
        try:
            self._keys.trusted_peer(
                hello.host_key_id, hello.host_key_generation
            )
            local = self._keys.local_identity()
        except ProtocolError as exc:
            self._record_failure(peer_binding, hello.host_key_id)
            raise AuthenticationError("host key is not authorized") from exc
        session_id = self._next_session_id
        self._next_session_id += 1
        if self._next_session_id > 0xFFFFFFFFFFFFFFFF:
            self._next_session_id = 0
        if session_id == 0:
            raise AuthenticationError("session ID space is exhausted")
        now = self._now_ms()
        challenge = AuthChallenge(
            hello.host_key_id,
            hello.host_key_generation,
            local.key_id,
            local.generation,
            self._generation,
            session_id,
            peer_binding,
            now,
            now + self._challenge_timeout_ms,
            hello.request_nonce,
            hello.client_nonce,
            self._nonce(NONCE_BYTES),
        )
        challenge = challenge.with_signature(
            self._keys.sign_local(challenge.transcript())
        )
        challenge.encode()
        self._request_replay.append(hello.request_nonce)
        self._pending[session_id] = _PendingChallenge(
            challenge, peer_binding
        )
        return challenge

    def authenticate(
        self, proof: AuthProof, *, peer_binding: int
    ) -> tuple[AuthResult, AuthenticatedSession | None]:
        if self._shutting_down:
            raise AuthenticationError("authentication authority is shut down")
        proof.encode()
        pending = self._pending.pop(proof.session_id, None)
        if pending is None:
            self._record_failure(peer_binding, proof.host_key_id)
            raise AuthenticationError("authentication challenge is missing")
        challenge = pending.challenge
        now = self._now_ms()
        valid = (
            pending.peer_binding == peer_binding
            and _challenge_matches_proof(challenge, proof)
            and now < challenge.challenge_expires_at_ms
            and now < proof.proof_expires_at_ms
        )
        if valid:
            try:
                valid = self._keys.verify_peer(
                    proof.host_key_id,
                    proof.host_key_generation,
                    proof.transcript(),
                    proof.signature,
                )
            except ProtocolError:
                valid = False
        if not valid:
            retry = self._record_failure(
                peer_binding, challenge.host_key_id
            )
            denied_proof = AuthProof.from_challenge(
                challenge,
                max(
                    challenge.server_time_ms + 1,
                    min(
                        proof.proof_expires_at_ms,
                        challenge.challenge_expires_at_ms,
                    ),
                ),
            )
            result = AuthResult.from_proof(
                denied_proof,
                status=STATUS_DENIED,
                retry_after_ms=retry,
                session_nonce=bytes(NONCE_BYTES),
            )
            result = result.with_signature(
                self._keys.sign_local(result.transcript())
            )
            return result, None
        if len(self._sessions) >= MAX_AUTHENTICATED_SESSIONS:
            raise AuthenticationError("authenticated session limit reached")
        session_nonce = self._nonce(NONCE_BYTES)
        session = AuthenticatedSession(
            proof.service_generation,
            proof.session_id,
            proof.transport_binding,
            proof.host_key_id,
            proof.host_key_generation,
            proof.server_key_id,
            proof.server_key_generation,
            session_nonce,
            proof.proof_expires_at_ms,
        )
        result = AuthResult.from_proof(
            proof,
            status=STATUS_OK,
            retry_after_ms=0,
            session_nonce=session_nonce,
        )
        result = result.with_signature(
            self._keys.sign_local(result.transcript())
        )
        self._sessions[session.session_id] = session
        self._failures.pop(
            self._failure_key(peer_binding, proof.host_key_id), None
        )
        return result, session

    def disconnect(self, session_id: int, *, peer_binding: int) -> bool:
        session = self._sessions.get(session_id)
        if session is None or session.transport_binding != peer_binding:
            return False
        del self._sessions[session_id]
        return True

    def session(self, session_id: int) -> AuthenticatedSession | None:
        session = self._sessions.get(session_id)
        if session is not None and self._now_ms() >= session.expires_at_ms:
            del self._sessions[session_id]
            return None
        return session

    def shutdown(self) -> None:
        with self._connection_lock:
            self._shutting_down = True
            connections = tuple(self._connections)
            self._connections.clear()
            self._pending.clear()
            self._sessions.clear()
        for stream in connections:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                stream.close()
            except OSError:
                pass

    def _register_connection(self, stream: socket.socket) -> None:
        with self._connection_lock:
            if self._shutting_down:
                raise AuthenticationError(
                    "authentication authority is shut down"
                )
            self._connections.add(stream)

    def _unregister_connection(self, stream: socket.socket) -> None:
        with self._connection_lock:
            self._connections.discard(stream)


class AuthenticatedAttachClient:
    """Host client that can authenticate only; no target operation exists."""

    def __init__(
        self,
        stream: socket.socket,
        key_store: KeyStore,
        *,
        operation_timeout: float = 3.0,
        proof_window_ms: int = 2_000,
        entropy: Callable[[int], bytes] = secrets.token_bytes,
    ) -> None:
        if (
            isinstance(proof_window_ms, bool)
            or not MIN_CHALLENGE_MS <= proof_window_ms <= MAX_PROOF_WINDOW_MS
        ):
            raise ProtocolError("proof window is outside fixed bounds")
        self._stream = stream
        self._keys = key_store
        self._timeout = checked_timeout(operation_timeout)
        self._proof_window_ms = proof_window_ms
        self._entropy = entropy
        self._session: AuthenticatedSession | None = None
        self._closed = False

    def _nonce(self) -> bytes:
        value = self._entropy(NONCE_BYTES)
        if (
            not isinstance(value, bytes)
            or len(value) != NONCE_BYTES
            or not any(value)
        ):
            raise ProtocolError("host entropy provider failed closed")
        return value

    def authenticate(self) -> AuthenticatedSession:
        if self._closed:
            raise ProtocolError("authenticated attach client is closed")
        if self._session is not None:
            raise ProtocolError("client is already authenticated")
        local = self._keys.local_identity()
        hello = AuthHello(
            local.key_id,
            local.generation,
            self._nonce(),
            self._nonce(),
        )
        deadline = time.monotonic() + self._timeout
        send_auth_record(self._stream, hello.encode(), deadline)
        challenge = AuthChallenge.decode(
            receive_auth_record(self._stream, deadline)
        )
        if (
            not _same_u64(challenge.host_key_id, hello.host_key_id)
            or not _same_u64(
                challenge.host_key_generation, hello.host_key_generation
            )
            or not _same_bytes(
                challenge.request_nonce, hello.request_nonce
            )
            or not _same_bytes(challenge.client_nonce, hello.client_nonce)
        ):
            raise AuthenticationError(
                "server challenge is not bound to this request"
            )
        if not self._keys.verify_peer(
            challenge.server_key_id,
            challenge.server_key_generation,
            challenge.transcript(),
            challenge.signature,
        ):
            raise AuthenticationError("server challenge signature is invalid")
        proof_expiry = min(
            challenge.challenge_expires_at_ms,
            challenge.server_time_ms + self._proof_window_ms,
        )
        proof = AuthProof.from_challenge(challenge, proof_expiry)
        proof = proof.with_signature(
            self._keys.sign_local(proof.transcript())
        )
        send_auth_record(self._stream, proof.encode(), deadline)
        result = AuthResult.decode(
            receive_auth_record(self._stream, deadline)
        )
        if not _proof_matches_result(proof, result):
            raise AuthenticationError(
                "authentication result is not bound to this proof"
            )
        if not self._keys.verify_peer(
            result.server_key_id,
            result.server_key_generation,
            result.transcript(),
            result.signature,
        ):
            raise AuthenticationError(
                "authentication result signature is invalid"
            )
        if result.status != STATUS_OK:
            if result.retry_after_ms:
                raise RateLimitError(result.retry_after_ms)
            raise AuthenticationError("server denied host authentication")
        self._session = AuthenticatedSession(
            result.service_generation,
            result.session_id,
            result.transport_binding,
            result.host_key_id,
            result.host_key_generation,
            result.server_key_id,
            result.server_key_generation,
            result.session_nonce,
            result.proof_expires_at_ms,
        )
        return self._session

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._session = None
        try:
            self._stream.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._stream.close()
        except OSError:
            pass

    def __enter__(self) -> "AuthenticatedAttachClient":
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        self.close()


def serve_authentication(
    authority: AuthenticationAuthority,
    stream: socket.socket,
    *,
    peer_binding: int,
    timeout: float = 3.0,
) -> AuthenticatedSession | None:
    """Serve exactly one bounded handshake; caller owns post-auth transport."""

    authority._register_connection(stream)
    try:
        deadline = time.monotonic() + checked_timeout(timeout)
        hello = AuthHello.decode(receive_auth_record(stream, deadline))
        challenge = authority.issue_challenge(
            hello, peer_binding=peer_binding
        )
        send_auth_record(stream, challenge.encode(), deadline)
        proof = AuthProof.decode(receive_auth_record(stream, deadline))
        result, session = authority.authenticate(
            proof, peer_binding=peer_binding
        )
        send_auth_record(stream, result.encode(), deadline)
        return session
    finally:
        authority._unregister_connection(stream)


assert MAX_AUTH_FRAME_SIZE <= 4096
