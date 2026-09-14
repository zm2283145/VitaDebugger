from __future__ import annotations

import re
import struct
import sys
import unittest
from pathlib import Path


ATTACH_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ATTACH_ROOT / "host"))

from vdattach.errors import ProtocolError  # noqa: E402
from vdattach.protocol import (  # noqa: E402
    CAP_EXACT_TITLE_DISCOVERY,
    CAP_IDENTITY_TICKET,
    CAP_STATUS,
    CAP_TICKET_RELEASE,
    CURRENT_KERNEL_ABI,
    MAX_FRAME_SIZE,
    READ_ONLY_CAPABILITIES,
    DiscoverResult,
    HelloResult,
    ReleaseResult,
    frame_record,
    make_discover_request,
    make_hello_request,
    make_release_request,
    parse_discover_request,
    parse_discover_result,
    parse_frame_prefix,
    parse_hello_request,
    parse_hello_result,
    parse_release_request,
    parse_release_result,
    validate_title_id,
)


REQUEST_ID = "11" * 16
CLIENT_NONCE = "22" * 32
SERVER_NONCE = "33" * 32
TARGET_TICKET = "44" * 32
SERVICE_GENERATION = 0x1020304050607080
TARGET_GENERATION = 0x1122334455667788


class ProtocolTests(unittest.TestCase):
    def test_hello_round_trip_is_canonical_and_observe_only(self):
        request = make_hello_request(
            request_id=REQUEST_ID,
            client_nonce=CLIENT_NONCE,
        )
        self.assertEqual(parse_hello_request(request.data), request)
        self.assertIn(b"mode=observe\n", request.data)

        result = HelloResult(
            request_id=REQUEST_ID,
            client_nonce=CLIENT_NONCE,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="ready",
            attach_caps=READ_ONLY_CAPABILITIES,
            kernel_abi=CURRENT_KERNEL_ABI,
            kernel_caps=0x8000000F,
            ticket_lease_ms=5000,
            message="ready on trusted LAN",
        )
        self.assertEqual(parse_hello_result(result.data), result)
        self.assertIn(b"control_policy=disabled\n", result.data)

    def test_discover_and_release_round_trip(self):
        request = make_discover_request(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            target_title_id="UVDBDEMO1",
        )
        self.assertEqual(parse_discover_request(request.data), request)
        self.assertNotIn(b"pid=", request.data)

        result = DiscoverResult(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="found",
            target_title_id="UVDBDEMO1",
            pid=0x00010005,
            main_modid=0x40001234,
            main_fingerprint=0xAABBCCDD,
            target_generation=TARGET_GENERATION,
            target_ticket=TARGET_TICKET,
            message="identity captured",
        )
        self.assertEqual(parse_discover_result(result.data), result)

        release = make_release_request(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            target_ticket=TARGET_TICKET,
        )
        self.assertEqual(parse_release_request(release.data), release)
        released = ReleaseResult(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            target_ticket=TARGET_TICKET,
            state="released",
        )
        self.assertEqual(parse_release_result(released.data), released)

    def test_failed_discovery_cannot_smuggle_identity_fields(self):
        invalid = DiscoverResult(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="denied",
            target_title_id="UVDBDEMO1",
            pid=0x10005,
            message="not allowlisted",
        )
        with self.assertRaisesRegex(ProtocolError, "must not carry"):
            _ = invalid.data

        valid = DiscoverResult(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="not_found",
            target_title_id="UVDBDEMO1",
            message="not running",
        )
        self.assertEqual(parse_discover_result(valid.data), valid)

        incomplete = DiscoverResult(
            request_id=REQUEST_ID,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="found",
            target_title_id="UVDBDEMO1",
            pid=0x10005,
            main_modid=0x40001234,
            main_fingerprint=0,
            target_generation=TARGET_GENERATION,
            target_ticket=TARGET_TICKET,
        )
        with self.assertRaisesRegex(ProtocolError, "nonzero main module fingerprint"):
            _ = incomplete.data

    def test_parser_rejects_mutating_mode_unknown_caps_and_noncanonical_text(self):
        request = make_hello_request(request_id=REQUEST_ID, client_nonce=CLIENT_NONCE)
        with self.assertRaisesRegex(ProtocolError, "mode must be observe"):
            parse_hello_request(request.data.replace(b"mode=observe", b"mode=attach"))

        with self.assertRaisesRegex(ProtocolError, "unsupported version-1 bits"):
            _ = make_hello_request(
                request_id=REQUEST_ID,
                client_nonce=CLIENT_NONCE,
                required_caps=1 << 31,
            )

        with self.assertRaises(ProtocolError):
            parse_hello_request(request.data.replace(b"\n", b"\r\n", 1))
        with self.assertRaises(ProtocolError):
            parse_hello_request(request.data + b"extra=value\n")
        with self.assertRaises(ProtocolError):
            parse_hello_request(request.data.replace(b"required_caps=", b"required_caps=0"))

        hello_result = HelloResult(
            request_id=REQUEST_ID,
            client_nonce=CLIENT_NONCE,
            server_nonce=SERVER_NONCE,
            service_generation=SERVICE_GENERATION,
            state="ready",
            attach_caps=READ_ONLY_CAPABILITIES,
            kernel_abi=CURRENT_KERNEL_ABI,
            kernel_caps=0,
            ticket_lease_ms=5000,
            message="A",
        ).data
        with self.assertRaisesRegex(ProtocolError, "canonically percent-encoded"):
            parse_hello_result(hello_result.replace(b"message=A", b"message=%41"))
        with self.assertRaisesRegex(ProtocolError, "non-printable"):
            parse_hello_request(request.data.replace(b"mode=observe", b"mode=ob\x0bserve"))
        with self.assertRaisesRegex(ProtocolError, "unsafe terminal"):
            _ = HelloResult(
                request_id=REQUEST_ID,
                client_nonce=CLIENT_NONCE,
                server_nonce=SERVER_NONCE,
                service_generation=SERVICE_GENERATION,
                state="ready",
                attach_caps=READ_ONLY_CAPABILITIES,
                kernel_abi=CURRENT_KERNEL_ABI,
                kernel_caps=0,
                ticket_lease_ms=5000,
                message="\x1b[31mspoofed",
            ).data
        with self.assertRaisesRegex(ProtocolError, "bidirectional"):
            parse_hello_result(hello_result.replace(
                b"message=A", b"message=%E2%80%AEspoofed"))
        for encoded_separator in (b"%E2%80%A8", b"%E2%80%A9"):
            with self.subTest(encoded_separator=encoded_separator):
                with self.assertRaisesRegex(ProtocolError, "unsafe terminal"):
                    parse_hello_result(hello_result.replace(
                        b"message=A", b"message=" + encoded_separator +
                        b"spoofed"))

    def test_title_and_frame_limits_fail_closed(self):
        self.assertEqual(validate_title_id("UVDBDEMO1"), "UVDBDEMO1")
        for value in ("", "uvdbdemo1", "TOO-LONG10", "SHELL", "AAAAAAAA\u00c9"):
            with self.subTest(value=value):
                with self.assertRaises(ProtocolError):
                    validate_title_id(value)

        payload = b"x" * MAX_FRAME_SIZE
        framed = frame_record(payload)
        self.assertEqual(parse_frame_prefix(framed[:4]), MAX_FRAME_SIZE)
        self.assertEqual(framed[4:], payload)
        for length in (0, MAX_FRAME_SIZE + 1, 0xFFFFFFFF):
            with self.subTest(length=length):
                with self.assertRaises(ProtocolError):
                    parse_frame_prefix(struct.pack(">I", length))

    def test_c_header_and_python_constants_stay_in_sync(self):
        header = (ATTACH_ROOT / "include" / "vitadebug_attach_protocol.h").read_text(
            encoding="utf-8"
        )
        expected = {
            "VD_ATTACH_WIRE_VERSION": 1,
            "VD_ATTACH_MAX_FRAME_SIZE": MAX_FRAME_SIZE,
            "VD_ATTACH_CURRENT_KERNEL_ABI": CURRENT_KERNEL_ABI,
            "VD_ATTACH_CAP_STATUS": CAP_STATUS,
            "VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY": CAP_EXACT_TITLE_DISCOVERY,
            "VD_ATTACH_CAP_IDENTITY_TICKET": CAP_IDENTITY_TICKET,
            "VD_ATTACH_CAP_TICKET_RELEASE": CAP_TICKET_RELEASE,
        }
        for name, value in expected.items():
            with self.subTest(name=name):
                match = re.search(
                    rf"^#define {name} (?:(?:\(1u << (\d+)\))|(0x[0-9A-Fa-f]+|\d+)u)$",
                    header,
                    re.MULTILINE,
                )
                self.assertIsNotNone(match)
                parsed = 1 << int(match.group(1)) if match.group(1) else int(match.group(2), 0)
                self.assertEqual(parsed, value)

        kernel_header = (
            ATTACH_ROOT.parent / "kernel" / "include" / "vitadebug_kernel.h"
        ).read_text(encoding="utf-8")
        kernel_match = re.search(
            r"^#define VD_KERNEL_ABI_VERSION (0x[0-9A-Fa-f]+)u$",
            kernel_header,
            re.MULTILINE,
        )
        self.assertIsNotNone(kernel_match)
        self.assertEqual(int(kernel_match.group(1), 0), CURRENT_KERNEL_ABI)


if __name__ == "__main__":
    unittest.main()
