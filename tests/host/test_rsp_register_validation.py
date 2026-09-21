import unittest

from tools import rsp_register_validation as registers


CORE = b"01234567" * 16
UNAVAILABLE = b"xx" * registers.LEGACY_FPA_BYTES
CPSR = b"00000080"
OBSERVED_SHAPE = CORE + UNAVAILABLE + CPSR


class RegisterPayloadTests(unittest.TestCase):
    def assert_rejected(self, payload: bytes) -> None:
        with self.assertRaises(registers.RegisterPayloadError):
            registers.require_legacy_register_bank_payload(payload, "registers")

    def test_observed_reply_shape_accepts_complete_unavailable_bytes(self) -> None:
        self.assertEqual(len(OBSERVED_SHAPE), 336)
        registers.require_legacy_register_bank_payload(
            OBSERVED_SHAPE, "observed retail reply"
        )

    def test_all_hex_reply_is_accepted(self) -> None:
        registers.require_legacy_register_bank_payload(
            b"aF" * registers.LEGACY_G_REPLY_BYTES, "all hex"
        )

    def test_unavailable_region_accepts_mixed_hex_and_xx_bytes(self) -> None:
        unavailable = b"".join(
            b"xx" if index % 2 else b"5A"
            for index in range(registers.LEGACY_FPA_BYTES)
        )
        registers.require_legacy_register_bank_payload(
            CORE + unavailable + CPSR, "mixed unavailable"
        )

    def test_lone_mixed_or_uppercase_x_markers_are_rejected(self) -> None:
        start = registers.CORE_REGISTER_BYTES * 2
        for pair in (b"x0", b"0x", b"xF", b"Fx", b"XX"):
            payload = bytearray(OBSERVED_SHAPE)
            payload[start:start + 2] = pair
            with self.subTest(pair=pair):
                self.assert_rejected(bytes(payload))

    def test_misaligned_x_across_byte_boundary_is_rejected(self) -> None:
        start = registers.CORE_REGISTER_BYTES * 2
        payload = bytearray(OBSERVED_SHAPE)
        payload[start - 1:start + 1] = b"xx"
        self.assert_rejected(bytes(payload))

    def test_xx_is_rejected_in_required_core_and_cpsr_bytes(self) -> None:
        for offset in (0, len(OBSERVED_SHAPE) - 2):
            payload = bytearray(OBSERVED_SHAPE)
            payload[offset:offset + 2] = b"xx"
            with self.subTest(offset=offset):
                self.assert_rejected(bytes(payload))

    def test_invalid_characters_are_rejected(self) -> None:
        payload = bytearray(OBSERVED_SHAPE)
        payload[registers.CORE_REGISTER_BYTES * 2] = ord("z")
        self.assert_rejected(bytes(payload))

    def test_odd_short_and_long_replies_are_rejected(self) -> None:
        for payload in (
            OBSERVED_SHAPE[:-1],
            OBSERVED_SHAPE[:-2],
            OBSERVED_SHAPE + b"0",
            OBSERVED_SHAPE + b"00",
        ):
            with self.subTest(size=len(payload)):
                self.assert_rejected(payload)


if __name__ == "__main__":
    unittest.main()
