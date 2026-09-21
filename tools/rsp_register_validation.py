import string


CORE_REGISTER_BYTES = 16 * 4
LEGACY_FPA_BYTES = (8 * 12) + 4
CPSR_BYTES = 4
LEGACY_G_REPLY_BYTES = CORE_REGISTER_BYTES + LEGACY_FPA_BYTES + CPSR_BYTES
HEX_BYTES = frozenset(string.hexdigits.encode("ascii"))


class RegisterPayloadError(ValueError):
    pass


def require_legacy_register_bank_payload(value: bytes, label: str) -> None:
    expected_size = LEGACY_G_REPLY_BYTES * 2
    if len(value) != expected_size:
        raise RegisterPayloadError(
            f"{label}: expected {expected_size} register characters, "
            f"got {len(value)}"
        )

    unavailable_start = CORE_REGISTER_BYTES * 2
    unavailable_end = unavailable_start + LEGACY_FPA_BYTES * 2
    for offset in range(0, expected_size, 2):
        pair = value[offset:offset + 2]
        if pair[0] in HEX_BYTES and pair[1] in HEX_BYTES:
            continue
        if unavailable_start <= offset < unavailable_end and pair == b"xx":
            continue
        raise RegisterPayloadError(
            f"{label}: invalid register byte at character {offset}"
        )
