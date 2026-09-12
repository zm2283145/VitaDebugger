from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.errors import ProtocolError
from vitadevdeploy.manifest import MANIFEST_HEADER, build_manifest, parse_manifest, verify_manifest
from vitadevdeploy.protocol import (
    Challenge,
    make_request,
    parse_challenge,
    parse_request,
    parse_result,
    percent_encode_message,
    signed_job_bytes,
)


class ManifestTests(unittest.TestCase):
    def test_manifest_is_path_sorted_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "z").write_bytes(b"z")
            (root / "a").write_bytes(b"a")
            first = build_manifest(root)
            second = build_manifest(root)
            self.assertEqual(first.data, second.data)
            self.assertEqual([entry.path for entry in first.entries], ["a", "z"])
            self.assertEqual(parse_manifest(first.data), first)

    def test_manifest_verification_detects_tampering(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "file"
            file.write_bytes(b"original")
            manifest = build_manifest(root)
            file.write_bytes(b"changed")
            with self.assertRaises(ProtocolError):
                verify_manifest(root, manifest.data)

    def test_parser_rejects_casefold_collision(self) -> None:
        digest = hashlib.sha256(b"").hexdigest()
        data = MANIFEST_HEADER + f"{digest}\t0\tFoo\n{digest}\t0\tfoo\n".encode("ascii")
        with self.assertRaises(ProtocolError):
            parse_manifest(data)


class ProtocolTests(unittest.TestCase):
    def test_challenge_and_request_round_trip(self) -> None:
        challenge = Challenge("ab" * 32)
        self.assertEqual(parse_challenge(challenge.data), challenge)
        request = make_request(
            job="12" * 16,
            nonce=challenge.nonce,
            action="verify",
            title_id="TEST00001",
            manifest_sha256="34" * 32,
            file_count=3,
            total_size=99,
        )
        self.assertEqual(parse_request(request.data), request)
        self.assertTrue(signed_job_bytes(request.data, b"manifest").startswith(b"VITADEVDEPLOY-SIGNED-JOB-1\x00"))

    def test_request_parser_does_not_assume_installer_identity(self) -> None:
        request = make_request(
            job="12" * 16,
            nonce="ab" * 32,
            action="install",
            title_id="VDEVDEP01",
            manifest_sha256="34" * 32,
            file_count=1,
            total_size=1,
        )
        self.assertEqual(parse_request(request.data).title_id, "VDEVDEP01")

    def test_result_parses_percent_encoded_utf8(self) -> None:
        message = percent_encode_message("promoted ✓")
        data = (
            "VITADEVDEPLOY-RESULT-1\n"
            f"job={'12' * 16}\n"
            "state=success\n"
            "stage=complete\n"
            "code=0\n"
            "title_id=TEST00001\n"
            f"message={message}\n"
        ).encode("ascii")
        result = parse_result(data)
        self.assertTrue(result.succeeded)
        self.assertEqual(result.message, "promoted ✓")

    def test_result_rejects_noncanonical_code(self) -> None:
        data = (
            "VITADEVDEPLOY-RESULT-1\n"
            f"job={'12' * 16}\nstate=failed\nstage=parse\ncode=-01\n"
            "title_id=TEST00001\nmessage=bad\n"
        ).encode("ascii")
        with self.assertRaises(ProtocolError):
            parse_result(data)


if __name__ == "__main__":
    unittest.main()
