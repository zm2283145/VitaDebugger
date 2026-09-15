from __future__ import annotations

import hashlib
import hmac
import re
import sys
import unittest
from dataclasses import replace
from pathlib import Path


ATTACH_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ATTACH_ROOT / "host"))

from vdattach.control_signing import (  # noqa: E402
    CONTROL_VERSION,
    FIXED_DEBUGGER_SLOT,
    OPERATION_AUTH_DOMAIN,
    OPERATION_ATTACH,
    OPERATION_DETACH,
    OPERATION_RECOVER,
    OPERATION_SIGNED_BYTES,
    PEER_AUTH_DOMAIN,
    PEER_SIGNED_BYTES,
    OperationAuthorization,
    PeerTranscript,
    TargetIdentity,
    encode_operation_authorization,
    encode_peer_transcript,
)
from vdattach.errors import ProtocolError  # noqa: E402


TEST_KEY = bytes(range(32))


def _peer() -> PeerTranscript:
    return PeerTranscript(
        service_generation=0x0102030405060708,
        session_id=0x1112131415161718,
        transport_binding=0x2122232425262728,
        host_key_id=0x3132333435363738,
        server_time_ms=0x0000018BCFE56800,
        challenge_expires_at_ms=0x0000018BCFE56BE8,
        expires_at_ms=0x0000018BCFE569F4,
        server_nonce=bytes(range(0x00, 0x20)),
        client_nonce=bytes(range(0x20, 0x40)),
    )


def _operation() -> OperationAuthorization:
    return OperationAuthorization(
        operation=OPERATION_DETACH,
        service_generation=0x0102030405060708,
        session_id=0x1112131415161718,
        transport_binding=0x2122232425262728,
        host_key_id=0x3132333435363738,
        expires_at_ms=0x0000018BCFE569F4,
        session_expires_at_ms=0x0000018BCFE58000,
        requested_lease_ms=0,
        lease_id=0x4142434445464748,
        lease_expires_at_ms=0x0000018BCFE57000,
        injected_module_uid=0x40000042,
        target=TargetIdentity(
            title_id="UVDBDEMO1",
            pid=0x00010005,
            main_modid=0x40001234,
            main_fingerprint=0xAABBCCDD,
            target_generation=0x5152535455565758,
        ),
        server_nonce=bytes(range(0x00, 0x20)),
        client_nonce=bytes(range(0x20, 0x40)),
        request_nonce=bytes(range(0x40, 0x60)),
    )


def _attach_operation(lease_ms: int) -> OperationAuthorization:
    return replace(
        _operation(),
        operation=OPERATION_ATTACH,
        requested_lease_ms=lease_ms,
        lease_id=0,
        lease_expires_at_ms=0,
        injected_module_uid=0,
    )


def _tag(payload: bytes) -> bytes:
    # Test-only 64-byte authenticator matching the production signature width.
    return hmac.digest(TEST_KEY, payload, hashlib.sha512)


def _flip(value: bytes) -> bytes:
    return bytes((value[0] ^ 1,)) + value[1:]


def _macro_text(path: Path, name: str) -> str:
    text = path.read_text(encoding="ascii")
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(?:\\\s*)?\"([^\"]+)\"",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"missing C macro {name}")
    return match.group(1)


def _macro_uint(path: Path, name: str) -> int:
    text = path.read_text(encoding="ascii")
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+([0-9]+)u$",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"missing C integer macro {name}")
    return int(match.group(1))


class ControlSigningTests(unittest.TestCase):
    def assert_payload_invalidates(self, expected_tag, encoder, value):
        try:
            payload = encoder(value)
        except ProtocolError:
            return
        self.assertFalse(hmac.compare_digest(expected_tag, _tag(payload)))

    def test_c_and_python_golden_vectors_match(self):
        wire_header = (
            ATTACH_ROOT / "broker" / "include" /
            "vitadebug_attach_control_wire.h"
        )
        vector_header = (
            ATTACH_ROOT / "broker" / "tests" / "control_golden_vectors.h"
        )
        lease_min = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_LEASE_MIN_MS"
        )
        lease_max = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_LEASE_MAX_MS"
        )
        self.assertEqual(lease_min, 250)
        self.assertEqual(lease_max, 60_000)
        self.assertEqual(
            _macro_text(wire_header, "VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN"),
            PEER_AUTH_DOMAIN.decode("ascii"),
        )
        self.assertEqual(
            _macro_text(
                wire_header, "VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN"
            ),
            OPERATION_AUTH_DOMAIN.decode("ascii"),
        )
        self.assertEqual(
            _macro_uint(wire_header, "VD_ATTACH_CONTROL_PEER_SIGNED_BYTES"),
            PEER_SIGNED_BYTES,
        )
        self.assertEqual(
            _macro_uint(
                wire_header, "VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES"
            ),
            OPERATION_SIGNED_BYTES,
        )
        self.assertEqual(
            encode_peer_transcript(_peer()).hex(),
            _macro_text(vector_header, "VD_ATTACH_CONTROL_GOLDEN_PEER_HEX"),
        )
        vectors = (
            (
                _attach_operation(lease_min),
                "VD_ATTACH_CONTROL_GOLDEN_ATTACH_MIN_HEX",
            ),
            (
                _attach_operation(lease_max),
                "VD_ATTACH_CONTROL_GOLDEN_ATTACH_MAX_HEX",
            ),
            (_operation(), "VD_ATTACH_CONTROL_GOLDEN_DETACH_HEX"),
            (
                replace(_operation(), operation=OPERATION_RECOVER),
                "VD_ATTACH_CONTROL_GOLDEN_RECOVER_HEX",
            ),
        )
        for authorization, macro in vectors:
            with self.subTest(macro=macro):
                self.assertEqual(
                    encode_operation_authorization(authorization).hex(),
                    _macro_text(vector_header, macro),
                )

    def test_integer_offsets_are_fixed_width_network_order(self):
        peer = encode_peer_transcript(_peer())
        domain = len(PEER_AUTH_DOMAIN)
        self.assertEqual(peer[domain : domain + 4], CONTROL_VERSION.to_bytes(4, "big"))
        self.assertEqual(peer[domain + 4 : domain + 12], bytes.fromhex("0102030405060708"))
        self.assertEqual(peer[domain + 12 : domain + 20], bytes.fromhex("1112131415161718"))
        self.assertEqual(peer[domain + 20 : domain + 28], bytes.fromhex("2122232425262728"))

        operation = encode_operation_authorization(_operation())
        domain = len(OPERATION_AUTH_DOMAIN)
        self.assertEqual(operation[domain : domain + 4], CONTROL_VERSION.to_bytes(4, "big"))
        self.assertEqual(operation[domain + 4 : domain + 8], OPERATION_DETACH.to_bytes(4, "big"))
        self.assertEqual(operation[domain + 8 : domain + 12], FIXED_DEBUGGER_SLOT.to_bytes(4, "big"))
        self.assertEqual(operation[domain + 84 : domain + 93], b"UVDBDEMO1")

    def test_peer_signature_covers_every_challenge_peer_time_and_nonce_field(self):
        original = _peer()
        payload = encode_peer_transcript(original)
        expected_tag = _tag(payload)
        mutations = (
            replace(original, version=2),
            replace(original, service_generation=original.service_generation ^ 1),
            replace(original, session_id=original.session_id ^ 1),
            replace(original, transport_binding=original.transport_binding ^ 1),
            replace(original, host_key_id=original.host_key_id ^ 1),
            replace(original, server_time_ms=original.server_time_ms + 1),
            replace(
                original,
                challenge_expires_at_ms=original.challenge_expires_at_ms + 1,
            ),
            replace(original, expires_at_ms=original.expires_at_ms + 1),
            replace(original, server_nonce=_flip(original.server_nonce)),
            replace(original, client_nonce=_flip(original.client_nonce)),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assert_payload_invalidates(
                    expected_tag, encode_peer_transcript, mutation
                )

        changed_signature = bytes((expected_tag[0] ^ 1,)) + expected_tag[1:]
        self.assertEqual(
            payload,
            encode_peer_transcript(replace(original, signature=changed_signature)),
        )
        self.assertFalse(hmac.compare_digest(changed_signature, _tag(payload)))

    def test_operation_signature_covers_every_target_lease_and_nonce_field(self):
        original = _operation()
        payload = encode_operation_authorization(original)
        expected_tag = _tag(payload)
        target = original.target
        mutations = (
            replace(original, version=2),
            replace(original, operation=OPERATION_RECOVER),
            replace(original, fixed_module_slot=original.fixed_module_slot ^ 1),
            replace(original, service_generation=original.service_generation ^ 1),
            replace(original, session_id=original.session_id ^ 1),
            replace(original, transport_binding=original.transport_binding ^ 1),
            replace(original, host_key_id=original.host_key_id ^ 1),
            replace(original, expires_at_ms=original.expires_at_ms + 1),
            replace(
                original,
                session_expires_at_ms=original.session_expires_at_ms + 1,
            ),
            replace(original, requested_lease_ms=1),
            replace(original, lease_id=original.lease_id ^ 1),
            replace(
                original,
                lease_expires_at_ms=original.lease_expires_at_ms + 1,
            ),
            replace(
                original,
                injected_module_uid=original.injected_module_uid + 1,
            ),
            replace(original, target=replace(target, title_id="UVDBDEM02")),
            replace(original, target=replace(target, pid=target.pid + 1)),
            replace(
                original, target=replace(target, main_modid=target.main_modid + 1)
            ),
            replace(
                original,
                target=replace(
                    target, main_fingerprint=target.main_fingerprint ^ 1
                ),
            ),
            replace(
                original,
                target=replace(
                    target, target_generation=target.target_generation + 1
                ),
            ),
            replace(original, server_nonce=_flip(original.server_nonce)),
            replace(original, client_nonce=_flip(original.client_nonce)),
            replace(original, request_nonce=_flip(original.request_nonce)),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assert_payload_invalidates(
                    expected_tag, encode_operation_authorization, mutation
                )

        attach = _attach_operation(1000)
        attach_tag = _tag(encode_operation_authorization(attach))
        self.assert_payload_invalidates(
            attach_tag,
            encode_operation_authorization,
            replace(attach, requested_lease_ms=1001),
        )

        changed_signature = bytes((expected_tag[0] ^ 1,)) + expected_tag[1:]
        self.assertEqual(
            payload,
            encode_operation_authorization(
                replace(original, signature=changed_signature)
            ),
        )
        self.assertFalse(hmac.compare_digest(changed_signature, _tag(payload)))

    def test_invalid_sceuid_boundaries_fail_closed(self):
        vector_header = (
            ATTACH_ROOT / "broker" / "tests" / "control_golden_vectors.h"
        )
        minimum = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_SCEUID_MIN_VALID"
        )
        maximum = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_SCEUID_MAX_VALID"
        )
        zero = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_ZERO"
        )
        high = _macro_uint(
            vector_header, "VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_HIGH"
        )
        self.assertEqual((minimum, maximum, zero, high),
                         (1, 0x7FFFFFFF, 0, 0x80000000))
        cleanup = _operation()
        target = cleanup.target
        for pid, main_modid, injected_uid in (
            (minimum, maximum, minimum),
            (maximum, minimum, maximum),
        ):
            encode_operation_authorization(
                replace(
                    cleanup,
                    target=replace(target, pid=pid, main_modid=main_modid),
                    injected_module_uid=injected_uid,
                )
            )
        invalid = (
            replace(cleanup, target=replace(target, pid=zero)),
            replace(cleanup, target=replace(target, pid=high)),
            replace(cleanup, target=replace(target, main_modid=zero)),
            replace(
                cleanup, target=replace(target, main_modid=high)
            ),
            replace(cleanup, injected_module_uid=zero),
            replace(cleanup, injected_module_uid=high),
            replace(cleanup, target=replace(target, pid=-1)),
            replace(cleanup, injected_module_uid=-1),
        )
        for authorization in invalid:
            with self.subTest(authorization=authorization):
                with self.assertRaises(ProtocolError):
                    encode_operation_authorization(authorization)

    def test_peer_and_operation_domains_are_not_interchangeable(self):
        peer_payload = encode_peer_transcript(_peer())
        operation_payload = encode_operation_authorization(_operation())
        peer_tag = _tag(peer_payload)
        operation_tag = _tag(operation_payload)
        self.assertNotEqual(PEER_AUTH_DOMAIN, OPERATION_AUTH_DOMAIN)
        self.assertFalse(hmac.compare_digest(peer_tag, operation_tag))
        self.assertFalse(
            hmac.compare_digest(peer_tag, _tag(operation_payload))
        )


if __name__ == "__main__":
    unittest.main()
