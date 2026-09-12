from __future__ import annotations

import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.errors import VpkValidationError
from vitadevdeploy.paths import normalize_package_path
from vitadevdeploy.vpk import VpkLimits, extract_vpk, inspect_vpk
try:
    from .helpers import make_sfo, make_vpk
except ImportError:
    from helpers import make_sfo, make_vpk


class VpkTests(unittest.TestCase):
    def test_validates_and_extracts_safe_vpk(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vpk = make_vpk(root / "test.vpk", extra={"assets/model.bin": b"model"})
            inspection = inspect_vpk(vpk)
            self.assertEqual(inspection.title_id, "TEST00001")
            destination = root / "package"
            extracted = extract_vpk(vpk, destination)
            self.assertEqual(extracted.file_count, 3)
            self.assertEqual((destination / "assets" / "model.bin").read_bytes(), b"model")

    def test_rejects_parent_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.vpk"
            make_vpk(path, extra={"../escape": b"bad"})
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path)

    def test_rejects_backslash_alias(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.vpk"
            # zipfile normalizes backslashes while writing on Windows. Patch
            # the equal-length name in both local and central headers so the
            # validator sees the malicious archive spelling.
            make_vpk(path, extra={"assets/escape": b"bad"})
            path.write_bytes(path.read_bytes().replace(b"assets/escape", b"assets\\escape"))
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path)

    def test_rejects_casefold_duplicate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.vpk"
            make_vpk(path, extra={"Assets/file": b"one", "assets/FILE": b"two"})
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path)

    def test_rejects_symlink_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.vpk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("eboot.bin", b"x")
                archive.writestr("sce_sys/param.sfo", make_sfo())
                link = zipfile.ZipInfo("link")
                link.create_system = 3
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(link, b"target")
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path)

    def test_enforces_count_and_size_limits(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = make_vpk(Path(directory) / "large.vpk", extra={"extra": b"1234"})
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path, VpkLimits(max_entries=2))
            with self.assertRaises(VpkValidationError):
                inspect_vpk(path, VpkLimits(max_file_size=3))

    def test_path_limit_is_240_ascii_bytes(self) -> None:
        self.assertEqual(len(normalize_package_path("a" * 240).value), 240)
        with self.assertRaises(VpkValidationError):
            normalize_package_path("a" * 241)


if __name__ == "__main__":
    unittest.main()
