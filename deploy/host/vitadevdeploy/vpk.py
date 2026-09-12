"""Fail-closed VPK validation and extraction."""

from __future__ import annotations

import os
import shutil
import stat
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

from .errors import VpkValidationError
from .paths import NormalizedPath, normalize_package_path
from .sfo import SfoMetadata, parse_sfo


@dataclass(frozen=True)
class VpkLimits:
    max_entries: int = 8192
    max_file_size: int = 256 * 1024 * 1024
    max_total_size: int = 1024 * 1024 * 1024
    max_compression_ratio: int = 1000

    def validate(self) -> None:
        if min(self.max_entries, self.max_file_size, self.max_total_size, self.max_compression_ratio) <= 0:
            raise ValueError("all VPK limits must be positive")


@dataclass(frozen=True)
class VpkEntry:
    archive_name: str
    path: str
    collision_key: str
    is_directory: bool
    size: int


@dataclass(frozen=True)
class VpkInspection:
    title_id: str
    content_id: str | None
    entries: tuple[VpkEntry, ...]
    file_count: int
    total_size: int


def _validate_zip(zf: zipfile.ZipFile, limits: VpkLimits) -> tuple[list[tuple[zipfile.ZipInfo, NormalizedPath]], SfoMetadata, int]:
    limits.validate()
    infos = zf.infolist()
    if not infos:
        raise VpkValidationError("VPK is empty")
    if len(infos) > limits.max_entries:
        raise VpkValidationError(f"VPK has {len(infos)} entries; limit is {limits.max_entries}")

    validated: list[tuple[zipfile.ZipInfo, NormalizedPath]] = []
    seen: dict[str, bool] = {}
    file_keys: set[str] = set()
    total_size = 0
    sfo_info: zipfile.ZipInfo | None = None
    saw_eboot = False

    for info in infos:
        # ZipInfo.filename is normalized with the host OS separator. Validate
        # orig_filename so a backslash in an archive cannot be silently turned
        # into a safe-looking slash before our checks run.
        archive_name = info.orig_filename
        if info.flag_bits & 0x1:
            raise VpkValidationError(f"encrypted ZIP entry is forbidden: {archive_name!r}")
        if info.file_size < 0 or info.compress_size < 0:
            raise VpkValidationError(f"ZIP entry has a negative size: {archive_name!r}")
        is_directory = info.is_dir() or archive_name.endswith("/")
        normalized = normalize_package_path(archive_name, directory=is_directory)
        if normalized.collision_key in seen:
            raise VpkValidationError(f"duplicate normalized package path: {normalized.value!r}")

        unix_mode = (info.external_attr >> 16) & 0xFFFF
        file_type = stat.S_IFMT(unix_mode)
        if file_type and file_type not in {stat.S_IFREG, stat.S_IFDIR}:
            raise VpkValidationError(f"special or symbolic-link ZIP entry is forbidden: {archive_name!r}")
        if file_type == stat.S_IFDIR and not is_directory:
            raise VpkValidationError(f"ZIP entry type conflicts with its name: {archive_name!r}")

        for parent_index in range(1, len(normalized.collision_key.split("/"))):
            parent = "/".join(normalized.collision_key.split("/")[:parent_index])
            if parent in file_keys:
                raise VpkValidationError(f"file/directory path collision at {normalized.value!r}")
        if not is_directory:
            prefix = normalized.collision_key + "/"
            if any(existing.startswith(prefix) for existing in seen):
                raise VpkValidationError(f"file/directory path collision at {normalized.value!r}")
            file_keys.add(normalized.collision_key)
            if info.file_size > limits.max_file_size:
                raise VpkValidationError(f"ZIP entry exceeds the per-file limit: {normalized.value!r}")
            total_size += info.file_size
            if total_size > limits.max_total_size:
                raise VpkValidationError("VPK exceeds the total uncompressed-size limit")
            if info.file_size > 1024 * 1024:
                denominator = max(info.compress_size, 1)
                if info.file_size > denominator * limits.max_compression_ratio:
                    raise VpkValidationError(f"ZIP entry exceeds the compression-ratio limit: {normalized.value!r}")

        seen[normalized.collision_key] = is_directory
        validated.append((info, normalized))
        if normalized.value == "sce_sys/param.sfo" and not is_directory:
            sfo_info = info
        if normalized.value == "eboot.bin" and not is_directory:
            saw_eboot = True

    if sfo_info is None:
        raise VpkValidationError("VPK does not contain root sce_sys/param.sfo")
    if not saw_eboot:
        raise VpkValidationError("VPK does not contain root eboot.bin")
    try:
        sfo_bytes = zf.read(sfo_info)
    except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
        raise VpkValidationError("could not safely read sce_sys/param.sfo") from exc
    metadata = parse_sfo(sfo_bytes)
    return validated, metadata, total_size


def inspect_vpk(path: os.PathLike[str] | str, limits: VpkLimits | None = None) -> VpkInspection:
    selected_limits = limits or VpkLimits()
    try:
        with zipfile.ZipFile(path, "r") as zf:
            entries, metadata, total_size = _validate_zip(zf, selected_limits)
    except (OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
        if isinstance(exc, VpkValidationError):
            raise
        raise VpkValidationError(f"could not open VPK: {exc}") from exc
    public_entries = tuple(
        VpkEntry(
            archive_name=info.orig_filename,
            path=normalized.value,
            collision_key=normalized.collision_key,
            is_directory=info.is_dir() or info.orig_filename.endswith("/"),
            size=info.file_size,
        )
        for info, normalized in entries
    )
    return VpkInspection(
        title_id=metadata.title_id,
        content_id=metadata.content_id,
        entries=public_entries,
        file_count=sum(not entry.is_directory for entry in public_entries),
        total_size=total_size,
    )


def extract_vpk(
    path: os.PathLike[str] | str,
    destination: os.PathLike[str] | str,
    limits: VpkLimits | None = None,
) -> VpkInspection:
    """Validate fully, extract into a private temp directory, then rename."""

    selected_limits = limits or VpkLimits()
    target = Path(destination)
    if target.exists():
        raise VpkValidationError(f"extraction destination already exists: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    temp_root = Path(tempfile.mkdtemp(prefix=f".{target.name}.extract-", dir=target.parent))
    try:
        with zipfile.ZipFile(path, "r") as zf:
            entries, metadata, total_size = _validate_zip(zf, selected_limits)
            copied_total = 0
            for info, normalized in entries:
                output = temp_root.joinpath(*normalized.value.split("/"))
                if info.is_dir() or info.orig_filename.endswith("/"):
                    output.mkdir(parents=True, exist_ok=True)
                    if not output.is_dir():
                        raise VpkValidationError(f"directory extraction collision: {normalized.value!r}")
                    continue
                output.parent.mkdir(parents=True, exist_ok=True)
                copied = 0
                with zf.open(info, "r") as source, output.open("xb") as sink:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        copied += len(chunk)
                        copied_total += len(chunk)
                        if copied > info.file_size or copied > selected_limits.max_file_size:
                            raise VpkValidationError(f"ZIP entry expanded beyond its declared limit: {normalized.value!r}")
                        if copied_total > selected_limits.max_total_size:
                            raise VpkValidationError("VPK expanded beyond its total-size limit")
                        sink.write(chunk)
                if copied != info.file_size:
                    raise VpkValidationError(f"ZIP entry size changed during extraction: {normalized.value!r}")
        os.replace(temp_root, target)
        public_entries = tuple(
            VpkEntry(
                archive_name=info.orig_filename,
                path=normalized.value,
                collision_key=normalized.collision_key,
                is_directory=info.is_dir() or info.orig_filename.endswith("/"),
                size=info.file_size,
            )
            for info, normalized in entries
        )
        return VpkInspection(
            title_id=metadata.title_id,
            content_id=metadata.content_id,
            entries=public_entries,
            file_count=sum(not entry.is_directory for entry in public_entries),
            total_size=total_size,
        )
    except Exception as exc:
        shutil.rmtree(temp_root, ignore_errors=True)
        if isinstance(exc, VpkValidationError):
            raise
        if isinstance(exc, (OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError)):
            raise VpkValidationError(f"VPK extraction failed safely: {exc}") from exc
        raise
