from __future__ import annotations

import os
import re
import socket
import sys
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path


ATTACH_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = ATTACH_ROOT.parent
sys.path.insert(0, str(ATTACH_ROOT / "host"))
sys.path.insert(0, str(REPO_ROOT / "deploy" / "host"))

from vdattach.auth import (  # noqa: E402
    AuthenticationAuthority,
    AuthenticatedAttachClient,
    receive_auth_record,
    serve_authentication,
)
from vdattach.auth_keys import JsonKeyStore, UnavailableKeyStore  # noqa: E402
from vdattach.auth_protocol import (  # noqa: E402
    MAX_AUTH_FRAME_SIZE,
    NONCE_BYTES,
    STATUS_DENIED,
    STATUS_OK,
    CLIENT_PROOF_DOMAIN,
    SERVER_CHALLENGE_DOMAIN,
    SESSION_RESULT_DOMAIN,
    AuthChallenge,
    AuthHello,
    AuthProof,
    AuthResult,
    frame_record,
    parse_frame_prefix,
)
from vdattach.cli import build_parser  # noqa: E402
from vdattach.errors import (  # noqa: E402
    AuthenticationError,
    ProtocolError,
    RateLimitError,
    TransportError,
)
from vdattach.protocol import (  # noqa: E402
    CONTROL_POLICY_DISABLED,
    WIRE_VERSION as READ_ONLY_WIRE_VERSION,
)
from vitadevdeploy.crypto import get_backend  # noqa: E402


HOST_KEY_ID = 0x0102030405060708
SERVER_KEY_ID = 0x1112131415161718
PEER_BINDING = 0x2122232425262728


class Clock:
    def __init__(self, value: int = 10_000):
        self.value = value

    def __call__(self) -> int:
        return self.value


class Entropy:
    def __init__(self, seed: int):
        self.value = seed

    def __call__(self, size: int) -> bytes:
        self.value = (self.value + 1) & 0xFF
        if self.value == 0:
            self.value = 1
        return bytes(
            ((self.value + offset) & 0xFF) or 1
            for offset in range(size)
        )


class StorePair:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        backend = get_backend("auto")
        self.host = JsonKeyStore(root / "host", backend)
        self.server = JsonKeyStore(root / "server", backend)
        host_identity = self.host.provision(local_key_id=HOST_KEY_ID)
        server_identity = self.server.provision(local_key_id=SERVER_KEY_ID)
        self.host.allow_peer(
            key_id=server_identity.key_id,
            generation=server_identity.generation,
            public_pem=server_identity.public_pem,
        )
        self.server.allow_peer(
            key_id=host_identity.key_id,
            generation=host_identity.generation,
            public_pem=host_identity.public_pem,
        )

    def close(self) -> None:
        self.temporary.cleanup()


def make_hello(entropy: Entropy | None = None) -> AuthHello:
    source = entropy or Entropy(0x30)
    return AuthHello(
        HOST_KEY_ID,
        1,
        source(NONCE_BYTES),
        source(NONCE_BYTES),
    )


def signed_proof(
    keys: JsonKeyStore,
    challenge: AuthChallenge,
    *,
    expires_at_ms: int | None = None,
) -> AuthProof:
    proof = AuthProof.from_challenge(
        challenge,
        expires_at_ms or challenge.server_time_ms + 1_000,
    )
    return proof.with_signature(keys.sign_local(proof.transcript()))


class AuthenticationProtocolTests(unittest.TestCase):
    def test_c_and_python_wire_contract_constants_match(self):
        header = (
            ATTACH_ROOT
            / "broker"
            / "include"
            / "vitadebug_attach_auth.h"
        ).read_text(encoding="ascii")
        integers = {
            name: int(value)
            for name, value in re.findall(
                r"^#define\s+(VD_ATTACH_AUTH_[A-Z_]+)\s+([0-9]+)u$",
                header,
                re.MULTILINE,
            )
        }
        self.assertEqual(integers["VD_ATTACH_AUTH_WIRE_VERSION"], 2)
        self.assertEqual(
            integers["VD_ATTACH_AUTH_FRAME_MAX_BYTES"],
            MAX_AUTH_FRAME_SIZE,
        )
        self.assertEqual(integers["VD_ATTACH_AUTH_HELLO_BYTES"], 88)
        self.assertEqual(integers["VD_ATTACH_AUTH_CHALLENGE_BYTES"], 240)
        self.assertEqual(integers["VD_ATTACH_AUTH_PROOF_BYTES"], 248)
        self.assertEqual(integers["VD_ATTACH_AUTH_RESULT_BYTES"], 288)
        for macro, domain in (
            (
                "VD_ATTACH_AUTH_SERVER_CHALLENGE_DOMAIN",
                SERVER_CHALLENGE_DOMAIN,
            ),
            ("VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN", CLIENT_PROOF_DOMAIN),
            (
                "VD_ATTACH_AUTH_SESSION_RESULT_DOMAIN",
                SESSION_RESULT_DOMAIN,
            ),
        ):
            match = re.search(
                rf"#define\s+{macro}\s+\\\s*\n\s*\"([^\"]+)\"",
                header,
            )
            self.assertIsNotNone(match)
            assert match is not None
            self.assertEqual(match.group(1), domain.decode("ascii"))

    def test_fixed_records_and_transcripts_are_canonical(self):
        hello = make_hello()
        self.assertEqual(AuthHello.decode(hello.encode()), hello)
        challenge = AuthChallenge(
            HOST_KEY_ID,
            1,
            SERVER_KEY_ID,
            1,
            3,
            4,
            PEER_BINDING,
            100,
            200,
            hello.request_nonce,
            hello.client_nonce,
            bytes(range(1, 33)),
            bytes(range(64)),
        )
        self.assertEqual(
            AuthChallenge.decode(challenge.encode()), challenge
        )
        proof = AuthProof.from_challenge(challenge, 150).with_signature(
            bytes(range(64))
        )
        self.assertEqual(AuthProof.decode(proof.encode()), proof)
        result = AuthResult.from_proof(
            proof,
            status=STATUS_OK,
            retry_after_ms=0,
            session_nonce=bytes(range(32, 64)),
        ).with_signature(bytes(range(64)))
        self.assertEqual(AuthResult.decode(result.encode()), result)
        self.assertNotEqual(challenge.transcript(), proof.transcript())
        self.assertNotEqual(proof.transcript(), result.transcript())
        self.assertLess(len(result.encode()), MAX_AUTH_FRAME_SIZE)

    def test_malformed_oversized_and_noncanonical_records_fail_closed(self):
        hello = make_hello().encode()
        cases = (
            b"",
            hello[:-1],
            b"BAD!" + hello[4:],
            hello[:4] + b"\x03" + hello[5:],
            hello[:6] + b"\x00\x01" + hello[8:],
            hello + b"\x00",
        )
        for data in cases:
            with self.subTest(size=len(data)):
                with self.assertRaises(ProtocolError):
                    AuthHello.decode(data)
        for size in (0, MAX_AUTH_FRAME_SIZE + 1, 0xFFFFFFFF):
            with self.subTest(size=size):
                with self.assertRaises(ProtocolError):
                    parse_frame_prefix(size.to_bytes(4, "big"))
        with self.assertRaises(ProtocolError):
            frame_record(bytes(MAX_AUTH_FRAME_SIZE + 1))

    def test_v1_is_permanently_isolated_from_authenticated_v2(self):
        self.assertEqual(READ_ONLY_WIRE_VERSION, 1)
        self.assertEqual(CONTROL_POLICY_DISABLED, "disabled")
        parser = build_parser()
        command_action = next(
            action for action in parser._actions if action.dest == "command"
        )
        self.assertEqual(set(command_action.choices), {"status", "discover"})
        for command in command_action.choices.values():
            destinations = {action.dest for action in command._actions}
            self.assertTrue(
                {"pid", "module_path", "address", "operation"}.isdisjoint(
                    destinations
                )
            )


class AuthenticationAuthorityTests(unittest.TestCase):
    def setUp(self):
        self.keys = StorePair()
        self.clock = Clock()
        self.authority = AuthenticationAuthority(
            self.keys.server,
            now_ms=self.clock,
            entropy=Entropy(0x40),
        )

    def tearDown(self):
        self.keys.close()

    def test_mutual_authentication_and_disconnect(self):
        challenge = self.authority.issue_challenge(
            make_hello(), peer_binding=PEER_BINDING
        )
        self.assertTrue(
            self.keys.host.verify_peer(
                challenge.server_key_id,
                challenge.server_key_generation,
                challenge.transcript(),
                challenge.signature,
            )
        )
        result, session = self.authority.authenticate(
            signed_proof(self.keys.host, challenge),
            peer_binding=PEER_BINDING,
        )
        self.assertEqual(result.status, STATUS_OK)
        self.assertIsNotNone(session)
        assert session is not None
        self.assertEqual(
            self.authority.session(session.session_id), session
        )
        self.assertTrue(
            self.keys.host.verify_peer(
                result.server_key_id,
                result.server_key_generation,
                result.transcript(),
                result.signature,
            )
        )
        self.assertTrue(
            self.authority.disconnect(
                session.session_id, peer_binding=PEER_BINDING
            )
        )
        self.assertIsNone(self.authority.session(session.session_id))

    def test_wrong_key_signature_peer_and_stale_generation_are_denied(self):
        cases = ("signature", "peer", "generation", "service_generation")
        for case in cases:
            with self.subTest(case=case):
                self.clock.value += 10_000
                challenge = self.authority.issue_challenge(
                    make_hello(Entropy(self.clock.value & 0xFF)),
                    peer_binding=PEER_BINDING,
                )
                proof = signed_proof(self.keys.host, challenge)
                binding = PEER_BINDING
                if case == "signature":
                    proof = replace(
                        proof,
                        signature=bytes((proof.signature[0] ^ 1,))
                        + proof.signature[1:],
                    )
                elif case == "peer":
                    binding += 1
                elif case == "generation":
                    proof = replace(
                        proof,
                        host_key_generation=proof.host_key_generation + 1,
                    )
                else:
                    proof = replace(
                        proof,
                        service_generation=proof.service_generation + 1,
                    )
                result, session = self.authority.authenticate(
                    proof, peer_binding=binding
                )
                self.assertEqual(result.status, STATUS_DENIED)
                self.assertIsNone(session)
                self.assertFalse(any(result.session_nonce))

    def test_request_replay_and_rate_limit_backoff(self):
        hello = make_hello()
        self.authority.issue_challenge(
            hello, peer_binding=PEER_BINDING
        )
        with self.assertRaises(AuthenticationError):
            self.authority.issue_challenge(
                hello, peer_binding=PEER_BINDING
            )
        with self.assertRaises(RateLimitError) as blocked:
            self.authority.issue_challenge(
                make_hello(Entropy(0x60)), peer_binding=PEER_BINDING
            )
        self.assertGreaterEqual(blocked.exception.retry_after_ms, 1)
        first_delay = blocked.exception.retry_after_ms
        self.clock.value += first_delay
        challenge = self.authority.issue_challenge(
            make_hello(Entropy(0x70)), peer_binding=PEER_BINDING
        )
        bad = signed_proof(self.keys.host, challenge)
        bad = replace(
            bad,
            signature=bytes((bad.signature[0] ^ 1,)) + bad.signature[1:],
        )
        result, _ = self.authority.authenticate(
            bad, peer_binding=PEER_BINDING
        )
        self.assertEqual(result.status, STATUS_DENIED)
        self.assertGreater(result.retry_after_ms, first_delay)

    def test_timeout_shutdown_and_expired_session_are_deterministic(self):
        challenge = self.authority.issue_challenge(
            make_hello(), peer_binding=PEER_BINDING
        )
        self.clock.value = challenge.challenge_expires_at_ms
        result, session = self.authority.authenticate(
            signed_proof(
                self.keys.host,
                challenge,
                expires_at_ms=challenge.challenge_expires_at_ms,
            ),
            peer_binding=PEER_BINDING,
        )
        self.assertEqual(result.status, STATUS_DENIED)
        self.assertIsNone(session)

        self.clock.value += result.retry_after_ms
        challenge = self.authority.issue_challenge(
            make_hello(Entropy(0x73)), peer_binding=PEER_BINDING
        )
        result, session = self.authority.authenticate(
            signed_proof(self.keys.host, challenge),
            peer_binding=PEER_BINDING,
        )
        assert session is not None
        self.clock.value = session.expires_at_ms
        self.assertIsNone(self.authority.session(session.session_id))
        self.authority.shutdown()
        self.assertIsNone(self.authority.session(session.session_id))
        with self.assertRaises(AuthenticationError):
            self.authority.issue_challenge(
                make_hello(Entropy(0x77)), peer_binding=PEER_BINDING
            )

    def test_unavailable_device_storage_fails_closed(self):
        authority = AuthenticationAuthority(
            UnavailableKeyStore(),
            now_ms=self.clock,
            entropy=Entropy(0x55),
        )
        with self.assertRaises(AuthenticationError):
            authority.issue_challenge(
                make_hello(), peer_binding=PEER_BINDING
            )

    def test_rotation_and_revocation_change_authorization_immediately(self):
        rotated = self.keys.host.rotate_local(new_key_id=HOST_KEY_ID + 1)
        rotated_hello = AuthHello(
            rotated.key_id,
            rotated.generation,
            Entropy(0x81)(NONCE_BYTES),
            Entropy(0x82)(NONCE_BYTES),
        )
        with self.assertRaises(AuthenticationError):
            self.authority.issue_challenge(
                rotated_hello, peer_binding=PEER_BINDING
            )
        self.clock.value += 1_000
        self.keys.server.allow_peer(
            key_id=rotated.key_id,
            generation=rotated.generation,
            public_pem=rotated.public_pem,
        )
        challenge = self.authority.issue_challenge(
            replace(
                rotated_hello,
                request_nonce=Entropy(0x83)(NONCE_BYTES),
            ),
            peer_binding=PEER_BINDING,
        )
        result, session = self.authority.authenticate(
            signed_proof(self.keys.host, challenge),
            peer_binding=PEER_BINDING,
        )
        self.assertEqual(result.status, STATUS_OK)
        self.assertIsNotNone(session)
        self.keys.server.revoke_peer(
            key_id=rotated.key_id, generation=rotated.generation
        )
        self.assertTrue(
            self.authority.disconnect(
                challenge.session_id, peer_binding=PEER_BINDING
            )
        )
        with self.assertRaises(AuthenticationError):
            self.authority.issue_challenge(
                replace(
                    rotated_hello,
                    request_nonce=Entropy(0x84)(NONCE_BYTES),
                ),
                peer_binding=PEER_BINDING,
            )


class AuthenticationClientTests(unittest.TestCase):
    def setUp(self):
        self.keys = StorePair()
        self.clock = Clock()
        self.authority = AuthenticationAuthority(
            self.keys.server,
            now_ms=self.clock,
            entropy=Entropy(0x20),
        )

    def tearDown(self):
        self.keys.close()

    def test_bounded_socket_handshake_and_client_close(self):
        client_socket, server_socket = socket.socketpair()
        outcome: list[object] = []

        def server() -> None:
            try:
                outcome.append(
                    serve_authentication(
                        self.authority,
                        server_socket,
                        peer_binding=PEER_BINDING,
                    )
                )
            except BaseException as exc:
                outcome.append(exc)
            finally:
                server_socket.close()

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        with AuthenticatedAttachClient(
            client_socket,
            self.keys.host,
            entropy=Entropy(0x10),
        ) as client:
            session = client.authenticate()
            self.assertEqual(session.transport_binding, PEER_BINDING)
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(outcome), 1)
        self.assertNotIsInstance(outcome[0], BaseException)
        self.assertTrue(
            self.authority.disconnect(
                session.session_id, peer_binding=PEER_BINDING
            )
        )

    def test_partial_oversized_timeout_and_disconnect_frames(self):
        for payload, expected in (
            (b"\x00", TransportError),
            (
                (MAX_AUTH_FRAME_SIZE + 1).to_bytes(4, "big"),
                ProtocolError,
            ),
            (b"\x00\x00\x00\x20" + b"\x00" * 3, TransportError),
        ):
            with self.subTest(payload=payload):
                client, server = socket.socketpair()
                with client, server:
                    server.sendall(payload)
                    if payload == b"\x00":
                        deadline = time.monotonic() + 0.03
                    else:
                        server.shutdown(socket.SHUT_WR)
                        deadline = time.monotonic() + 0.2
                    with self.assertRaises(expected):
                        receive_auth_record(client, deadline)

    def test_wrong_server_key_fails_before_host_proof(self):
        other = StorePair()
        client_socket, server_socket = socket.socketpair()
        error: list[BaseException] = []

        def server() -> None:
            try:
                hello = AuthHello.decode(
                    receive_auth_record(
                        server_socket, time.monotonic() + 2
                    )
                )
                challenge = self.authority.issue_challenge(
                    hello, peer_binding=PEER_BINDING
                )
                from vdattach.auth import send_auth_record

                send_auth_record(
                    server_socket,
                    challenge.encode(),
                    time.monotonic() + 2,
                )
                receive_auth_record(server_socket, time.monotonic() + 2)
            except BaseException as exc:
                error.append(exc)
            finally:
                server_socket.close()

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        try:
            with AuthenticatedAttachClient(
                client_socket,
                other.host,
                entropy=Entropy(0x33),
            ) as client:
                with self.assertRaises(AuthenticationError):
                    client.authenticate()
        finally:
            thread.join(timeout=3)
            other.close()
        self.assertFalse(thread.is_alive())
        self.assertTrue(error)

    def test_shutdown_cancels_blocked_handshake(self):
        client_socket, server_socket = socket.socketpair()
        error: list[BaseException] = []

        def server() -> None:
            try:
                serve_authentication(
                    self.authority,
                    server_socket,
                    peer_binding=PEER_BINDING,
                    timeout=30,
                )
            except BaseException as exc:
                error.append(exc)

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        client_socket.sendall(b"\x00")
        time.sleep(0.05)
        self.authority.shutdown()
        thread.join(timeout=1)
        client_socket.close()
        self.assertFalse(thread.is_alive())
        self.assertTrue(error)
        self.assertIsInstance(error[0], TransportError)


class KeyManagementTests(unittest.TestCase):
    def test_provision_rotation_revocation_and_tamper_fail_closed(self):
        pair = StorePair()
        try:
            original = pair.host.local_identity()
            rotated = pair.host.rotate_local(new_key_id=HOST_KEY_ID + 1)
            self.assertEqual(rotated.generation, original.generation + 1)
            self.assertNotEqual(rotated.key_id, original.key_id)
            self.assertFalse(
                (pair.host.root / f"local-{original.key_id:016x}-"
                 f"{original.generation}.private.pem").exists()
            )

            server = pair.server.local_identity()
            pair.host.revoke_peer(
                key_id=server.key_id, generation=server.generation
            )
            with self.assertRaisesRegex(ProtocolError, "revoked"):
                pair.host.trusted_peer(
                    server.key_id, server.generation
                )

            state_path = pair.server.root / "state.json"
            state_path.write_text('{"schema":1}\n', encoding="ascii")
            with self.assertRaises(ProtocolError):
                pair.server.local_identity()
        finally:
            pair.close()

    @unittest.skipUnless(os.name != "nt", "symlink semantics vary on Windows")
    def test_key_symlinks_are_rejected(self):
        pair = StorePair()
        try:
            identity = pair.host.local_identity()
            public_path = (
                pair.host.root
                / f"local-{identity.key_id:016x}-"
                f"{identity.generation}.public.pem"
            )
            target = pair.host.root / "moved.pem"
            public_path.replace(target)
            public_path.symlink_to(target)
            with self.assertRaisesRegex(ProtocolError, "not a link"):
                pair.host.local_identity()
        finally:
            pair.close()


if __name__ == "__main__":
    unittest.main()
