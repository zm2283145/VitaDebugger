from __future__ import annotations

import hashlib
import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR_PATH = PROJECT_ROOT / "tools" / "validate_ed25519_public_key.py"
SPEC = importlib.util.spec_from_file_location("vdd_key_validator", VALIDATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)

BASE_POINT = "58" + "66" * 31
IDENTITY = "01" + "00" * 31
ORDER_FOUR = "00" * 32
# The Ed25519 base point plus the order-two point (0, -1). It is a valid,
# canonical curve point but has a torsion component and is not in the
# prime-order subgroup.
BASE_POINT_PLUS_TORSION = "95" + "99" * 31
NON_CANONICAL_Y = "ed" + "ff" * 30 + "7f"


class Ed25519BuildKeyTests(unittest.TestCase):
    def test_accepts_a_non_identity_prime_subgroup_point(self) -> None:
        raw = bytes.fromhex(BASE_POINT)
        self.assertEqual(
            validator.validate_public_key_hex(BASE_POINT),
            hashlib.sha256(raw).hexdigest(),
        )

    def test_rejects_low_order_and_noncanonical_encodings(self) -> None:
        for encoded in (IDENTITY, ORDER_FOUR, NON_CANONICAL_Y):
            with self.subTest(encoded=encoded):
                with self.assertRaises(validator.PublicKeyValidationError):
                    validator.validate_public_key_hex(encoded)

    def test_rejects_a_valid_curve_point_outside_the_prime_subgroup(self) -> None:
        with self.assertRaisesRegex(
            validator.PublicKeyValidationError, "prime-order subgroup"
        ):
            validator.validate_public_key_hex(BASE_POINT_PLUS_TORSION)

    def test_command_reports_only_the_public_key_fingerprint(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(VALIDATOR_PATH), BASE_POINT],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            completed.stdout.strip(), hashlib.sha256(bytes.fromhex(BASE_POINT)).hexdigest()
        )
        self.assertEqual(completed.stderr, "")


if __name__ == "__main__":
    unittest.main()
