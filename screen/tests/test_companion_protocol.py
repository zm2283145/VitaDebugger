from __future__ import annotations

import hashlib
import struct
import unittest

from vdscreen.companion import (
    CAP_APP_INPUT,
    CAP_DEBUG_FILES,
    CAP_INPUT_PLAYBACK,
    CAP_INPUT_RECORD,
    CAP_SCREEN,
    CAP_STATUS,
    CompanionIdentity,
    CompanionProtocolError,
    HEADER_SIZE,
    MAC_DOMAIN,
    MESSAGE_HELLO,
    MESSAGE_STATUS,
    RESPONSE_BIT,
    decode_record,
    decode_status,
    decode_endpoint_config,
    encode_endpoint_config,
    encode_record,
    validate_response,
)


SECRET = bytes(range(1, 33))
IDENTITY = CompanionIdentity(
    "VDSCRN001", 42, 7, 0x1122334455667788)
CAPABILITIES = CAP_STATUS | CAP_APP_INPUT


def _record(**overrides) -> bytes:
    values = {
        "message_type": MESSAGE_HELLO,
        "sequence": 1,
        "ttl_ms": 500,
        "session_id": IDENTITY.session_id,
        "process_generation": IDENTITY.process_generation,
        "capabilities": CAPABILITIES,
    }
    values.update(overrides)
    return encode_record(SECRET, **values)


def _retag(data: bytearray) -> None:
    payload = bytes(data[HEADER_SIZE:])
    data[64:80] = hashlib.blake2b(
        MAC_DOMAIN + bytes(data[:64]) + payload,
        digest_size=16,
        key=SECRET,
    ).digest()


class CompanionProtocolTests(unittest.TestCase):
    def test_canonical_record_round_trip_and_tag_vector(self) -> None:
        data = _record(payload=b"abc")
        self.assertEqual(
            data.hex(),
            "564443500001005000010000000000030000000000000001"
            "00000000000001f411223344556677880000000000000007"
            "00000000000000030000000000000000"
            "fe622c46520b161b60e087b7d2ca1465616263",
        )
        decoded = decode_record(SECRET, data)
        self.assertEqual(decoded.payload, b"abc")
        self.assertEqual(decoded.sequence, 1)
        self.assertEqual(decoded.ttl_ms, 500)

    def test_response_binding_and_status_decode(self) -> None:
        payload = struct.pack(
            ">9s3xIQQQIIQQII",
            b"VDSCRN001",
            IDENTITY.process_id,
            IDENTITY.process_generation,
            IDENTITY.session_id,
            CAPABILITIES,
            2,
            4,
            5000,
            99,
            1,
            12,
        )
        response = decode_record(
            SECRET,
            _record(
                message_type=MESSAGE_STATUS | RESPONSE_BIT,
                sequence=2,
                status=12,
                payload=payload,
            ),
        )
        validate_response(
            response,
            request_type=MESSAGE_STATUS,
            sequence=2,
            identity=IDENTITY,
            capabilities=CAPABILITIES,
        )
        status = decode_status(response.payload)
        self.assertEqual(status.identity, IDENTITY)
        self.assertTrue(status.input_cleanup_pending)
        self.assertEqual(status.last_error, 12)
        self.assertEqual(status.trace_event_count, 4)

    def test_malformed_auth_bounds_and_reserved_fail_closed(self) -> None:
        canonical = _record(payload=b"abc")
        for length in range(HEADER_SIZE):
            with self.subTest(length=length), self.assertRaises(
                    CompanionProtocolError):
                decode_record(SECRET, canonical[:length])
        changed = bytearray(canonical)
        changed[64] ^= 1
        with self.assertRaisesRegex(CompanionProtocolError, "authentication"):
            decode_record(SECRET, bytes(changed))
        changed = bytearray(canonical)
        changed[63] = 1
        _retag(changed)
        with self.assertRaisesRegex(CompanionProtocolError, "reserved"):
            decode_record(SECRET, bytes(changed))
        changed = bytearray(canonical)
        struct.pack_into(">I", changed, 12, 4)
        _retag(changed)
        with self.assertRaisesRegex(CompanionProtocolError, "size"):
            decode_record(SECRET, bytes(changed))

    def test_invalid_ttl_identity_sequence_and_secret_are_rejected(self) -> None:
        for changes in (
            {"ttl_ms": 0},
            {"ttl_ms": 5001},
            {"sequence": 0},
            {"session_id": 0},
            {"process_generation": 0},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                _record(**changes)
        with self.assertRaises(ValueError):
            decode_record(bytes(32), _record())

    def test_response_correlation_rejects_stale_target(self) -> None:
        response = decode_record(
            SECRET,
            _record(
                message_type=MESSAGE_STATUS | RESPONSE_BIT,
                sequence=2,
            ),
        )
        cases = (
            {"sequence": 3},
            {"request_type": MESSAGE_HELLO},
            {
                "identity": CompanionIdentity(
                    IDENTITY.title_id,
                    IDENTITY.process_id,
                    IDENTITY.process_generation + 1,
                    IDENTITY.session_id,
                )
            },
            {"capabilities": CAP_STATUS},
        )
        defaults = {
            "request_type": MESSAGE_STATUS,
            "sequence": 2,
            "identity": IDENTITY,
            "capabilities": CAPABILITIES,
        }
        for change in cases:
            arguments = defaults | change
            with self.subTest(change=change), self.assertRaises(
                    CompanionProtocolError):
                validate_response(response, **arguments)

    def test_endpoint_config_has_independent_consents(self) -> None:
        capabilities = CAP_STATUS | CAP_DEBUG_FILES | CAP_INPUT_RECORD
        data = encode_endpoint_config(
            host="127.0.0.1",
            capabilities=capabilities,
            control_secret=SECRET,
            screen_secret=bytes(range(33, 65)),
        )
        config = decode_endpoint_config(data)
        self.assertEqual(config.capabilities, capabilities)
        self.assertEqual(config.host.compressed, "127.0.0.1")
        self.assertEqual(struct.unpack_from(">I", data, 16)[0], 0)
        self.assertNotEqual(struct.unpack_from(">I", data, 20)[0], 0)
        self.assertEqual(struct.unpack_from(">I", data, 24)[0], 0)

        playback = encode_endpoint_config(
            host="192.168.1.20",
            bind_address="192.168.1.30",
            allow_lan=True,
            capabilities=CAP_STATUS | CAP_SCREEN | CAP_INPUT_PLAYBACK,
            control_secret=SECRET,
            screen_secret=bytes(range(33, 65)),
        )
        self.assertNotEqual(struct.unpack_from(">I", playback, 12)[0], 0)
        self.assertNotEqual(struct.unpack_from(">I", playback, 16)[0], 0)
        self.assertEqual(struct.unpack_from(">I", playback, 20)[0], 0)
        self.assertNotEqual(struct.unpack_from(">I", playback, 24)[0], 0)
        self.assertEqual(
            decode_endpoint_config(playback).capabilities,
            CAP_STATUS | CAP_SCREEN | CAP_INPUT_PLAYBACK,
        )
        self.assertEqual(
            decode_endpoint_config(playback).bind_address.compressed,
            "192.168.1.30",
        )

    def test_endpoint_config_rejects_public_reuse_and_corruption(self) -> None:
        with self.assertRaises(ValueError):
            encode_endpoint_config(
                host="8.8.8.8",
                bind_address="192.168.1.30",
                allow_lan=True,
                capabilities=CAP_STATUS,
                control_secret=SECRET,
                screen_secret=bytes(range(33, 65)),
            )
        with self.assertRaises(ValueError):
            encode_endpoint_config(
                host="169.254.1.20",
                bind_address="192.168.1.30",
                allow_lan=True,
                capabilities=CAP_STATUS,
                control_secret=SECRET,
                screen_secret=bytes(range(33, 65)),
            )
        with self.assertRaises(ValueError):
            encode_endpoint_config(
                host="192.168.1.20",
                allow_lan=True,
                capabilities=CAP_STATUS,
                control_secret=SECRET,
                screen_secret=bytes(range(33, 65)),
            )
        with self.assertRaises(ValueError):
            encode_endpoint_config(
                host="127.0.0.1",
                capabilities=CAP_STATUS,
                control_secret=SECRET,
                screen_secret=SECRET,
            )
        data = bytearray(encode_endpoint_config(
            host="127.0.0.1",
            capabilities=CAP_STATUS | CAP_APP_INPUT,
            control_secret=SECRET,
            screen_secret=bytes(range(33, 65)),
        ))
        for offset in (0, 8, 16, 52, 54, 120, 127):
            changed = bytearray(data)
            changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(
                    CompanionProtocolError):
                decode_endpoint_config(bytes(changed))


if __name__ == "__main__":
    unittest.main()
