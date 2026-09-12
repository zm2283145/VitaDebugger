from __future__ import annotations

import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.errors import SfoFormatError, VpkValidationError
from vitadevdeploy.headbin import fpkg_hmac, generate_head_bin
from vitadevdeploy.sfo import parse_sfo
try:
    from .helpers import make_head_template, make_sfo
except ImportError:
    from helpers import make_head_template, make_sfo


class SfoTests(unittest.TestCase):
    def test_parses_title_and_content_id(self) -> None:
        metadata = parse_sfo(make_sfo("ABCD12345", "EP9000-ABCD12345_00-EXAMPLE000000000"))
        self.assertEqual(metadata.title_id, "ABCD12345")
        self.assertTrue(metadata.content_id.startswith("EP9000-"))

    def test_rejects_bad_title_id(self) -> None:
        with self.assertRaises(SfoFormatError):
            parse_sfo(make_sfo("bad"))

    def test_rejects_truncated_index(self) -> None:
        broken = bytearray(make_sfo())
        struct.pack_into("<I", broken, 16, 100)
        with self.assertRaises(SfoFormatError):
            parse_sfo(bytes(broken))


class HeadBinTests(unittest.TestCase):
    def test_fpkg_hmac_is_deterministic(self) -> None:
        expected = fpkg_hmac(b"example")
        self.assertEqual(expected, fpkg_hmac(b"example"))
        self.assertEqual(len(expected), 16)
        self.assertNotEqual(expected, hashlib.sha1(b"example").digest()[:16])

    def test_generates_content_and_all_three_digests(self) -> None:
        template = make_head_template()
        generated = generate_head_bin(template, make_sfo("TEST00001"))
        content = b"EP9000-TEST00001_00-0000000000000000"
        self.assertEqual(generated[0x30 : 0x30 + len(content)], content)
        self.assertEqual(generated[0x100:0x110], fpkg_hmac(generated[:0x100]))
        self.assertEqual(generated[0x180:0x190], fpkg_hmac(generated[0x120:0x160]))
        self.assertEqual(generated[0x200:0x210], fpkg_hmac(generated[:0x200]))

    def test_rejects_out_of_bounds_template(self) -> None:
        with self.assertRaises(VpkValidationError):
            generate_head_bin(b"short", make_sfo())


if __name__ == "__main__":
    unittest.main()
