"""Canonical path and identifier validation."""

from __future__ import annotations

import re
from dataclasses import dataclass

from .errors import ProtocolError, VpkValidationError

TITLE_ID_RE = re.compile(r"[A-Z0-9]{9}\Z")
JOB_ID_RE = re.compile(r"[0-9a-f]{32}\Z")
NONCE_RE = re.compile(r"[0-9a-f]{64}\Z")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")

_WINDOWS_INVALID = frozenset('<>"|?*')
_WINDOWS_RESERVED = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}


@dataclass(frozen=True)
class NormalizedPath:
    """A safe package-relative path and its collision key."""

    value: str
    collision_key: str


def validate_title_id(value: str) -> str:
    if not TITLE_ID_RE.fullmatch(value):
        raise ProtocolError("TITLE_ID must be exactly nine uppercase ASCII letters or digits")
    return value


def validate_job_id(value: str) -> str:
    if not JOB_ID_RE.fullmatch(value):
        raise ProtocolError("job id must be exactly 32 lowercase hexadecimal characters")
    return value


def validate_nonce(value: str) -> str:
    if not NONCE_RE.fullmatch(value):
        raise ProtocolError("nonce must be exactly 64 lowercase hexadecimal characters")
    return value


def validate_sha256(value: str) -> str:
    if not SHA256_RE.fullmatch(value):
        raise ProtocolError("SHA-256 must be exactly 64 lowercase hexadecimal characters")
    return value


def normalize_package_path(raw: str, *, directory: bool = False) -> NormalizedPath:
    """Validate a portable, manifest-safe package path.

    The wire format intentionally permits printable ASCII only. Backslashes and
    colons are rejected instead of being normalized because accepting aliases is
    how traversal and duplicate-path bugs enter archive extractors.
    """

    if not isinstance(raw, str) or not raw:
        raise VpkValidationError("package path is empty")
    if "\x00" in raw:
        raise VpkValidationError("package path contains a NUL byte")
    if directory:
        if not raw.endswith("/"):
            raise VpkValidationError(f"directory entry lacks a trailing slash: {raw!r}")
        raw = raw[:-1]
    elif raw.endswith("/"):
        raise VpkValidationError(f"file entry has a trailing slash: {raw!r}")
    if not raw or raw.startswith("/") or raw.startswith("\\"):
        raise VpkValidationError(f"absolute or empty package path is forbidden: {raw!r}")
    if "\\" in raw or ":" in raw:
        raise VpkValidationError(f"package path contains a forbidden separator: {raw!r}")
    if any(ord(char) < 0x20 or ord(char) > 0x7E for char in raw):
        raise VpkValidationError(f"package path must contain printable ASCII only: {raw!r}")
    if len(raw.encode("ascii")) > 240:
        raise VpkValidationError(f"package path exceeds 240 ASCII bytes: {raw!r}")

    parts = raw.split("/")
    for part in parts:
        if part in {"", ".", ".."}:
            raise VpkValidationError(f"package path contains an empty or dot segment: {raw!r}")
        if part[-1] in {" ", "."}:
            raise VpkValidationError(f"package path has a Windows-ambiguous segment: {raw!r}")
        if any(char in _WINDOWS_INVALID for char in part):
            raise VpkValidationError(f"package path is not portable to Windows: {raw!r}")
        stem = part.split(".", 1)[0].casefold()
        if stem in _WINDOWS_RESERVED:
            raise VpkValidationError(f"package path uses a reserved Windows name: {raw!r}")

    value = "/".join(parts)
    return NormalizedPath(value=value, collision_key=value.casefold())
