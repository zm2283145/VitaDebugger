from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.crypto import (
    export_public_key,
    generate_keypair,
    public_key_header,
    sign,
    verify,
)


def find_openssl() -> str | None:
    configured = os.environ.get("VITADEVDEPLOY_OPENSSL") or shutil.which("openssl")
    if configured:
        return configured
    windows_candidate = Path(r"C:\msys64\usr\bin\openssl.exe")
    return str(windows_candidate) if windows_candidate.is_file() else None


class CryptoTests(unittest.TestCase):
    def test_explicit_openssl_sign_verify_and_raw_export(self) -> None:
        openssl = find_openssl()
        if not openssl:
            self.skipTest("OpenSSL Ed25519 provider is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private = root / "developer-private.pem"
            public = root / "developer-public.pem"
            raw = root / "developer-public.bin"
            header = root / "developer-public.h"
            backend = generate_keypair(
                private,
                public,
                backend="openssl",
                openssl_executable=openssl,
            )
            self.assertEqual(backend, "openssl")
            signature = sign(private, b"signed job", backend="openssl", openssl_executable=openssl)
            self.assertTrue(
                verify(public, b"signed job", signature, backend="openssl", openssl_executable=openssl)
            )
            self.assertFalse(
                verify(public, b"tampered", signature, backend="openssl", openssl_executable=openssl)
            )
            exported = export_public_key(
                public,
                raw_path=raw,
                header_path=header,
                backend="openssl",
                openssl_executable=openssl,
            )
            self.assertEqual(len(exported), 32)
            self.assertEqual(raw.read_bytes(), exported)
            self.assertEqual(header.read_bytes(), public_key_header(exported))
            self.assertIn(b"vitadevdeploy_public_key[32]", header.read_bytes())


if __name__ == "__main__":
    unittest.main()
