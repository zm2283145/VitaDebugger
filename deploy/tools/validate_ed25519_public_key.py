"""Validate a raw Ed25519 public key for agent builds.

The verifier used on the Vita accepts an encoded curve point.  Agent builds
must be stricter: the configured trust anchor must be a canonical, non-identity
point in Ed25519's prime-order subgroup.  Keeping this check in a small
standard-library-only host tool lets CMake reject unsafe keys before compiling
or packaging an agent.
"""

from __future__ import annotations

import argparse
import hashlib
import sys


FIELD_PRIME = 2**255 - 19
GROUP_ORDER = 2**252 + 27742317777372353535851937790883648493
CURVE_D = (-121665 * pow(121666, FIELD_PRIME - 2, FIELD_PRIME)) % FIELD_PRIME
SQRT_MINUS_ONE = pow(2, (FIELD_PRIME - 1) // 4, FIELD_PRIME)

# Extended Edwards coordinates (X, Y, Z, T), with x=X/Z and y=Y/Z.
IDENTITY = (0, 1, 1, 0)


class PublicKeyValidationError(ValueError):
    """The supplied bytes are not a safe Ed25519 trust anchor."""


def _decode_point(raw: bytes) -> tuple[int, int, int, int]:
    if len(raw) != 32:
        raise PublicKeyValidationError("public key must contain exactly 32 bytes")

    encoded = int.from_bytes(raw, "little")
    sign = encoded >> 255
    y = encoded & ((1 << 255) - 1)
    if y >= FIELD_PRIME:
        raise PublicKeyValidationError("public key has a non-canonical field encoding")

    y_squared = y * y % FIELD_PRIME
    denominator = (CURVE_D * y_squared + 1) % FIELD_PRIME
    if denominator == 0:
        raise PublicKeyValidationError("public key does not encode an Ed25519 point")
    x_squared = (y_squared - 1) * pow(
        denominator, FIELD_PRIME - 2, FIELD_PRIME
    ) % FIELD_PRIME
    x = pow(x_squared, (FIELD_PRIME + 3) // 8, FIELD_PRIME)
    if (x * x - x_squared) % FIELD_PRIME != 0:
        x = x * SQRT_MINUS_ONE % FIELD_PRIME
    if (x * x - x_squared) % FIELD_PRIME != 0:
        raise PublicKeyValidationError("public key does not encode an Ed25519 point")
    if x == 0 and sign:
        raise PublicKeyValidationError("public key uses a non-canonical sign bit")
    if (x & 1) != sign:
        x = FIELD_PRIME - x
    return x, y, 1, x * y % FIELD_PRIME


def _add(
    left: tuple[int, int, int, int],
    right: tuple[int, int, int, int],
) -> tuple[int, int, int, int]:
    x1, y1, z1, t1 = left
    x2, y2, z2, t2 = right
    a = (y1 - x1) * (y2 - x2) % FIELD_PRIME
    b = (y1 + x1) * (y2 + x2) % FIELD_PRIME
    c = 2 * CURVE_D * t1 * t2 % FIELD_PRIME
    d = 2 * z1 * z2 % FIELD_PRIME
    e = (b - a) % FIELD_PRIME
    f = (d - c) % FIELD_PRIME
    g = (d + c) % FIELD_PRIME
    h = (b + a) % FIELD_PRIME
    return (
        e * f % FIELD_PRIME,
        g * h % FIELD_PRIME,
        f * g % FIELD_PRIME,
        e * h % FIELD_PRIME,
    )


def _double(point: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    x, y, z, _t = point
    a = x * x % FIELD_PRIME
    b = y * y % FIELD_PRIME
    c = 2 * z * z % FIELD_PRIME
    d = -a % FIELD_PRIME
    e = ((x + y) * (x + y) - a - b) % FIELD_PRIME
    g = (d + b) % FIELD_PRIME
    f = (g - c) % FIELD_PRIME
    h = (d - b) % FIELD_PRIME
    return (
        e * f % FIELD_PRIME,
        g * h % FIELD_PRIME,
        f * g % FIELD_PRIME,
        e * h % FIELD_PRIME,
    )


def _multiply(
    point: tuple[int, int, int, int], scalar: int
) -> tuple[int, int, int, int]:
    result = IDENTITY
    addend = point
    while scalar:
        if scalar & 1:
            result = _add(result, addend)
        addend = _double(addend)
        scalar >>= 1
    return result


def _is_identity(point: tuple[int, int, int, int]) -> bool:
    x, y, z, _t = point
    return x % FIELD_PRIME == 0 and (y - z) % FIELD_PRIME == 0


def validate_public_key(raw: bytes) -> str:
    """Return the SHA-256 fingerprint after strict subgroup validation."""

    point = _decode_point(raw)
    if _is_identity(point):
        raise PublicKeyValidationError("public key is the Ed25519 identity point")
    if not _is_identity(_multiply(point, GROUP_ORDER)):
        raise PublicKeyValidationError(
            "public key is not in Ed25519's prime-order subgroup"
        )
    return hashlib.sha256(raw).hexdigest()


def validate_public_key_hex(value: str) -> str:
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise PublicKeyValidationError(
            "public key must be exactly 64 lowercase hexadecimal characters"
        )
    return validate_public_key(bytes.fromhex(value))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("public_key_hex")
    args = parser.parse_args(argv)
    try:
        fingerprint = validate_public_key_hex(args.public_key_hex)
    except PublicKeyValidationError as exc:
        print(f"invalid Ed25519 public key: {exc}", file=sys.stderr)
        return 1
    print(fingerprint)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
