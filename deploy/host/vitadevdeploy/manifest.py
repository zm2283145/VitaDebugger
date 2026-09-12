"""Deterministic package manifests for the Vita agent."""

from __future__ import annotations

import hashlib
import os
import stat
from dataclasses import dataclass
from pathlib import Path

from .errors import ProtocolError, VpkValidationError
from .paths import normalize_package_path, validate_sha256

MANIFEST_HEADER = b"VITADEVDEPLOY-MANIFEST-1\n"
MANIFEST_MAX_FILES = 8192
MANIFEST_MAX_FILE_SIZE = 256 * 1024 * 1024
MANIFEST_MAX_TOTAL_SIZE = 1024 * 1024 * 1024


@dataclass(frozen=True)
class ManifestEntry:
    sha256: str
    size: int
    path: str


@dataclass(frozen=True)
class Manifest:
    data: bytes
    entries: tuple[ManifestEntry, ...]
    file_count: int
    total_size: int
    sha256: str


def sha256_file(path: os.PathLike[str] | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _canonical_uint(text: str, label: str) -> int:
    if text == "0":
        return 0
    if not text or text[0] == "0" or not text.isascii() or not text.isdecimal():
        raise ProtocolError(f"{label} is not a canonical unsigned decimal integer")
    return int(text, 10)


def build_manifest(
    package_root: os.PathLike[str] | str,
    *,
    max_files: int = 8192,
    max_total_size: int = 1024 * 1024 * 1024,
) -> Manifest:
    root = Path(package_root)
    if not root.is_dir() or root.is_symlink():
        raise VpkValidationError("package root must be a real directory")
    if max_files <= 0 or max_total_size <= 0:
        raise ValueError("manifest limits must be positive")

    candidates: list[tuple[str, Path]] = []
    collision_keys: set[str] = set()
    for current, directory_names, file_names in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        for name in directory_names:
            candidate = current_path / name
            if candidate.is_symlink():
                raise VpkValidationError(f"symbolic-link directory is forbidden: {candidate}")
        for name in file_names:
            candidate = current_path / name
            relative = candidate.relative_to(root).as_posix()
            normalized = normalize_package_path(relative)
            if normalized.value != relative:
                raise VpkValidationError(f"package path is not canonical: {relative!r}")
            if normalized.collision_key in collision_keys:
                raise VpkValidationError(f"case-folded package path collision: {relative!r}")
            collision_keys.add(normalized.collision_key)
            mode = candidate.lstat().st_mode
            if not stat.S_ISREG(mode):
                raise VpkValidationError(f"non-regular package file is forbidden: {relative!r}")
            candidates.append((relative, candidate))

    candidates.sort(key=lambda item: item[0])
    if len(candidates) > max_files:
        raise VpkValidationError(f"package has {len(candidates)} files; limit is {max_files}")

    entries: list[ManifestEntry] = []
    total_size = 0
    for relative, candidate in candidates:
        size = candidate.stat().st_size
        if size < 0:
            raise VpkValidationError(f"package file has a negative size: {relative!r}")
        total_size += size
        if total_size > max_total_size:
            raise VpkValidationError("package exceeds manifest total-size limit")
        entries.append(ManifestEntry(sha256=sha256_file(candidate), size=size, path=relative))

    rows = [MANIFEST_HEADER]
    rows.extend(f"{entry.sha256}\t{entry.size}\t{entry.path}\n".encode("ascii") for entry in entries)
    data = b"".join(rows)
    return Manifest(
        data=data,
        entries=tuple(entries),
        file_count=len(entries),
        total_size=total_size,
        sha256=hashlib.sha256(data).hexdigest(),
    )


def parse_manifest(data: bytes) -> Manifest:
    if not data.startswith(MANIFEST_HEADER) or len(data) > 4 * 1024 * 1024:
        raise ProtocolError("manifest header or size is invalid")
    if not data.endswith(b"\n"):
        raise ProtocolError("manifest must end with LF")
    try:
        text = data[len(MANIFEST_HEADER) :].decode("ascii", "strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError("manifest is not ASCII") from exc

    entries: list[ManifestEntry] = []
    seen: set[str] = set()
    previous_path: str | None = None
    total_size = 0
    for row in text.splitlines():
        pieces = row.split("\t")
        if len(pieces) != 3:
            raise ProtocolError("manifest row does not contain exactly three tab-separated fields")
        digest, size_text, path = pieces
        validate_sha256(digest)
        size = _canonical_uint(size_text, "manifest file size")
        if size > MANIFEST_MAX_FILE_SIZE:
            raise ProtocolError("manifest file exceeds the per-file size limit")
        try:
            normalized = normalize_package_path(path)
        except VpkValidationError as exc:
            raise ProtocolError(str(exc)) from exc
        if normalized.value != path:
            raise ProtocolError("manifest path is not canonical")
        if normalized.collision_key in seen:
            raise ProtocolError("manifest contains a case-folded path collision")
        if previous_path is not None and path <= previous_path:
            raise ProtocolError("manifest rows are not strictly sorted by path")
        seen.add(normalized.collision_key)
        previous_path = path
        total_size += size
        if total_size > MANIFEST_MAX_TOTAL_SIZE:
            raise ProtocolError("manifest exceeds the total-size limit")
        entries.append(ManifestEntry(sha256=digest, size=size, path=path))
        if len(entries) > MANIFEST_MAX_FILES:
            raise ProtocolError("manifest exceeds the file-count limit")
    return Manifest(
        data=data,
        entries=tuple(entries),
        file_count=len(entries),
        total_size=total_size,
        sha256=hashlib.sha256(data).hexdigest(),
    )


def verify_manifest(package_root: os.PathLike[str] | str, expected: bytes) -> Manifest:
    parsed = parse_manifest(expected)
    actual = build_manifest(package_root)
    if actual.data != parsed.data:
        raise ProtocolError("package contents do not match manifest")
    return actual
