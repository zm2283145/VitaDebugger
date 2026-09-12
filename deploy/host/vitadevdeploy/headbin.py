# SPDX-License-Identifier: GPL-3.0-only
"""Generate Vita ``head.bin`` from a VitaShell-compatible template.

The transformation and ``fpkg_hmac`` routine follow VitaShell's GPL-3.0
``package_installer.c``. The default bundled template and its provenance are
documented in ``THIRD_PARTY.md``; callers may supply an audited override.
"""

from __future__ import annotations

import hashlib
import os
import struct
import tempfile
from pathlib import Path

from .errors import VpkValidationError
from .sfo import parse_sfo

_CONTENT_ID_OFFSET = 0x30
_CONTENT_ID_SIZE = 48


def _be32(data: bytes | bytearray, offset: int, label: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise VpkValidationError(f"head.bin template is missing {label}")
    return struct.unpack_from(">I", data, offset)[0]


def _bounded(data: bytes | bytearray, start: int, length: int, label: str) -> None:
    if start < 0 or length < 0 or start > len(data) or length > len(data) - start:
        raise VpkValidationError(f"head.bin {label} points outside the template")


def fpkg_hmac(data: bytes) -> bytes:
    first = hashlib.sha1(data).digest()
    block = bytearray(64)
    block[0:8] = first[4:12]
    block[8:16] = first[4:12]
    block[16:20] = first[12:16]
    block[20:24] = bytes((first[16], first[1], first[2], first[3]))
    block[24:32] = block[16:24]
    return hashlib.sha1(block).digest()[:16]


def generate_head_bin(template: bytes, param_sfo: bytes) -> bytes:
    metadata = parse_sfo(param_sfo)
    if len(template) < _CONTENT_ID_OFFSET + _CONTENT_ID_SIZE:
        raise VpkValidationError("head.bin template is too short")
    content_id = metadata.content_id or f"EP9000-{metadata.title_id}_00-0000000000000000"
    try:
        encoded_content_id = content_id.encode("ascii", "strict")
    except UnicodeEncodeError as exc:
        raise VpkValidationError("CONTENT_ID is not ASCII") from exc
    if len(encoded_content_id) > _CONTENT_ID_SIZE:
        raise VpkValidationError("CONTENT_ID is longer than the head.bin field")

    output = bytearray(template)
    output[_CONTENT_ID_OFFSET : _CONTENT_ID_OFFSET + _CONTENT_ID_SIZE] = b"\x00" * _CONTENT_ID_SIZE
    output[_CONTENT_ID_OFFSET : _CONTENT_ID_OFFSET + len(encoded_content_id)] = encoded_content_id

    header_length = _be32(output, 0xD0, "header length")
    _bounded(output, 0, header_length, "header range")
    _bounded(output, header_length, 16, "header digest destination")
    output[header_length : header_length + 16] = fpkg_hmac(bytes(output[:header_length]))

    info_offset = _be32(output, 0x08, "package-info offset")
    info_length = _be32(output, 0x10, "package-info length")
    info_digest_offset = _be32(output, 0xD4, "package-info digest offset")
    if info_length < 64:
        raise VpkValidationError("head.bin package-info length is smaller than 64 bytes")
    _bounded(output, info_offset, info_length - 64, "package-info range")
    _bounded(output, info_digest_offset, 16, "package-info digest destination")
    output[info_digest_offset : info_digest_offset + 16] = fpkg_hmac(
        bytes(output[info_offset : info_offset + info_length - 64])
    )

    signed_length = _be32(output, 0xE8, "overall signed length")
    _bounded(output, 0, signed_length, "overall signed range")
    _bounded(output, signed_length, 16, "overall digest destination")
    output[signed_length : signed_length + 16] = fpkg_hmac(bytes(output[:signed_length]))
    return bytes(output)


def create_head_bin(
    package_root: os.PathLike[str] | str,
    template_path: os.PathLike[str] | str,
    *,
    overwrite: bool = False,
) -> Path:
    root = Path(package_root)
    sfo_path = root / "sce_sys" / "param.sfo"
    destination = root / "sce_sys" / "package" / "head.bin"
    if destination.exists() and not overwrite:
        raise VpkValidationError("VPK already contains sce_sys/package/head.bin")
    try:
        generated = generate_head_bin(Path(template_path).read_bytes(), sfo_path.read_bytes())
    except OSError as exc:
        raise VpkValidationError(f"could not read head.bin input: {exc}") from exc
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=".head.bin-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(generated)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, destination)
    except Exception:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise
    return destination
