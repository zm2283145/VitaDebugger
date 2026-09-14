#!/usr/bin/env python3
"""Versioned build identities for VitaDebugger symbol loading.

The identity file is a local deployment receipt.  It binds one exact VPK to
the unstripped ELF used for the main executable and, optionally, to the
unstripped ELF and packaged binary for each user module.  A live verifier can
then hash the corresponding installed files through Vita Companion FTP.

This module intentionally contains no deployment or mutation operations.
"""

from __future__ import annotations

import ftplib
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Mapping


IDENTITY_FORMAT = "VITADEBUGGER-BUILD-IDENTITY-1"
MAX_IDENTITY_BYTES = 64 * 1024
MAX_MODULES = 256
MAX_ARTIFACT_BYTES = 1024 * 1024 * 1024
FTP_BLOCK_SIZE = 64 * 1024

TITLE_ID_RE = re.compile(r"[A-Z0-9]{9}\Z")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
MODULE_NAME_RE = re.compile(r"[A-Za-z0-9_.+-]{1,64}\Z")


class BuildIdentityError(RuntimeError):
    """A build identity is missing, malformed, or does not match."""


def _json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise BuildIdentityError(f"duplicate JSON field {key!r} in build identity")
        result[key] = value
    return result


@dataclass(frozen=True)
class ArtifactIdentity:
    sha256: str
    size_bytes: int


@dataclass(frozen=True)
class InstalledArtifactIdentity:
    path: str
    sha256: str
    size_bytes: int


@dataclass(frozen=True)
class ModuleIdentity:
    name: str
    symbols: ArtifactIdentity
    installed: InstalledArtifactIdentity


@dataclass(frozen=True)
class BuildIdentity:
    title_id: str
    vpk: ArtifactIdentity
    main_symbols: ArtifactIdentity
    main_installed: InstalledArtifactIdentity
    modules: tuple[ModuleIdentity, ...]

    def module_map(self) -> dict[str, ModuleIdentity]:
        return {module.name: module for module in self.modules}


def _exact_keys(value: Mapping[str, object], expected: set[str], label: str) -> None:
    keys = set(value)
    if keys != expected:
        missing = ", ".join(sorted(expected - keys)) or "none"
        extra = ", ".join(sorted(keys - expected)) or "none"
        raise BuildIdentityError(
            f"{label} has unexpected fields (missing: {missing}; extra: {extra})"
        )


def _object(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise BuildIdentityError(f"{label} must be a JSON object")
    return value


def _canonical_size(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BuildIdentityError(f"{label} must be an integer")
    if not 0 < value <= MAX_ARTIFACT_BYTES:
        raise BuildIdentityError(f"{label} is outside the supported range")
    return value


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise BuildIdentityError(
            f"{label} must be exactly 64 lowercase hexadecimal characters"
        )
    return value


def validate_installed_path(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise BuildIdentityError("installed path must be 1-240 ASCII bytes")
    try:
        encoded = value.encode("ascii", "strict")
    except UnicodeEncodeError as exc:
        raise BuildIdentityError("installed path must contain ASCII only") from exc
    if len(encoded) > 240:
        raise BuildIdentityError("installed path must be 1-240 ASCII bytes")
    if (
        value.startswith(("/", "\\"))
        or value.endswith("/")
        or "\\" in value
        or ":" in value
        or any(ord(char) < 0x20 or ord(char) > 0x7E for char in value)
    ):
        raise BuildIdentityError("installed path is not a safe VPK-relative path")
    parts = value.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise BuildIdentityError("installed path contains an empty or dot segment")
    return value


def _artifact(value: object, label: str) -> ArtifactIdentity:
    record = _object(value, label)
    _exact_keys(record, {"sha256", "size_bytes"}, label)
    return ArtifactIdentity(
        _digest(record["sha256"], f"{label}.sha256"),
        _canonical_size(record["size_bytes"], f"{label}.size_bytes"),
    )


def _installed(value: object, label: str) -> InstalledArtifactIdentity:
    record = _object(value, label)
    _exact_keys(record, {"path", "sha256", "size_bytes"}, label)
    return InstalledArtifactIdentity(
        validate_installed_path(record["path"]),
        _digest(record["sha256"], f"{label}.sha256"),
        _canonical_size(record["size_bytes"], f"{label}.size_bytes"),
    )


def parse_build_identity(data: bytes | str) -> BuildIdentity:
    if isinstance(data, str):
        try:
            encoded = data.encode("utf-8", "strict")
        except UnicodeEncodeError as exc:
            raise BuildIdentityError("build identity is not valid UTF-8") from exc
    else:
        encoded = data
    if not encoded or len(encoded) > MAX_IDENTITY_BYTES:
        raise BuildIdentityError("build identity is empty or exceeds 64 KiB")
    try:
        decoded = encoded.decode("utf-8", "strict")
        root = json.loads(decoded, object_pairs_hook=_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BuildIdentityError("build identity is not valid UTF-8 JSON") from exc
    value = _object(root, "build identity")
    _exact_keys(value, {"format", "title_id", "vpk", "main", "modules"}, "build identity")
    if value["format"] != IDENTITY_FORMAT:
        raise BuildIdentityError(
            f"unsupported build identity format {value['format']!r}; "
            f"expected {IDENTITY_FORMAT!r}"
        )
    title_id = value["title_id"]
    if not isinstance(title_id, str) or TITLE_ID_RE.fullmatch(title_id) is None:
        raise BuildIdentityError("title_id must match [A-Z0-9]{9}")
    main = _object(value["main"], "main")
    _exact_keys(main, {"symbols", "installed"}, "main")
    raw_modules = value["modules"]
    if not isinstance(raw_modules, list) or len(raw_modules) > MAX_MODULES:
        raise BuildIdentityError("modules must be a JSON array with at most 256 entries")
    modules: list[ModuleIdentity] = []
    names: set[str] = set()
    installed_paths = {"eboot.bin".casefold()}
    for index, raw_module in enumerate(raw_modules):
        label = f"modules[{index}]"
        module = _object(raw_module, label)
        _exact_keys(module, {"name", "symbols", "installed"}, label)
        name = module["name"]
        if not isinstance(name, str) or MODULE_NAME_RE.fullmatch(name) is None:
            raise BuildIdentityError(f"{label}.name is not a safe runtime module name")
        if name in names:
            raise BuildIdentityError(f"duplicate module identity for {name!r}")
        names.add(name)
        installed = _installed(module["installed"], f"{label}.installed")
        folded_path = installed.path.casefold()
        if folded_path in installed_paths:
            raise BuildIdentityError(
                f"duplicate installed artifact path {installed.path!r}"
            )
        installed_paths.add(folded_path)
        modules.append(
            ModuleIdentity(
                name,
                _artifact(module["symbols"], f"{label}.symbols"),
                installed,
            )
        )
    main_installed = _installed(main["installed"], "main.installed")
    if main_installed.path != "eboot.bin":
        raise BuildIdentityError("main.installed.path must be exactly 'eboot.bin'")
    return BuildIdentity(
        title_id,
        _artifact(value["vpk"], "vpk"),
        _artifact(main["symbols"], "main.symbols"),
        main_installed,
        tuple(modules),
    )


def load_build_identity(path: Path | str) -> BuildIdentity:
    source = Path(path)
    try:
        before = source.stat().st_size
        if not source.is_file():
            raise BuildIdentityError("build identity path is not a regular file")
        if not 0 < before <= MAX_IDENTITY_BYTES:
            raise BuildIdentityError("build identity is empty or exceeds 64 KiB")
        data = source.read_bytes()
    except OSError as exc:
        raise BuildIdentityError(f"cannot read build identity: {exc}") from exc
    if len(data) != before:
        raise BuildIdentityError("build identity changed while it was read")
    return parse_build_identity(data)


def artifact_identity(path: Path | str) -> ArtifactIdentity:
    source = Path(path)
    try:
        before = source.stat().st_size
        if not source.is_file():
            raise BuildIdentityError(f"artifact is not a regular file: {source}")
        if not 0 < before <= MAX_ARTIFACT_BYTES:
            raise BuildIdentityError(f"artifact size is unsupported: {source}")
        digest = hashlib.sha256()
        total = 0
        with source.open("rb") as handle:
            while True:
                block = handle.read(1024 * 1024)
                if not block:
                    break
                total += len(block)
                if total > before:
                    raise BuildIdentityError(f"artifact grew while hashing: {source}")
                digest.update(block)
    except OSError as exc:
        raise BuildIdentityError(f"cannot hash artifact {source}: {exc}") from exc
    if total != before:
        raise BuildIdentityError(f"artifact changed while hashing: {source}")
    return ArtifactIdentity(digest.hexdigest(), total)


def require_artifact(path: Path | str, expected: ArtifactIdentity, label: str) -> None:
    actual = artifact_identity(path)
    if actual != expected:
        raise BuildIdentityError(
            f"{label} does not match the build identity "
            f"(expected {expected.sha256}/{expected.size_bytes}, "
            f"got {actual.sha256}/{actual.size_bytes})"
        )


def identity_to_json(identity: BuildIdentity) -> str:
    def artifact(value: ArtifactIdentity) -> dict[str, object]:
        return {"sha256": value.sha256, "size_bytes": value.size_bytes}

    def installed(value: InstalledArtifactIdentity) -> dict[str, object]:
        return {
            "path": value.path,
            "sha256": value.sha256,
            "size_bytes": value.size_bytes,
        }

    value = {
        "format": IDENTITY_FORMAT,
        "title_id": identity.title_id,
        "vpk": artifact(identity.vpk),
        "main": {
            "symbols": artifact(identity.main_symbols),
            "installed": installed(identity.main_installed),
        },
        "modules": [
            {
                "name": module.name,
                "symbols": artifact(module.symbols),
                "installed": installed(module.installed),
            }
            for module in sorted(identity.modules, key=lambda item: item.name)
        ],
    }
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def verify_local_identity(
    identity: BuildIdentity,
    vpk_path: Path | str,
    main_elf: Path | str,
    module_elfs: Mapping[str, Path | str],
) -> None:
    """Require the local artifacts to be exactly those in the receipt."""

    require_artifact(vpk_path, identity.vpk, "VPK")
    require_artifact(main_elf, identity.main_symbols, "main ELF")
    expected = identity.module_map()
    actual_names = set(module_elfs)
    expected_names = set(expected)
    if not actual_names <= expected_names:
        unrecorded = ", ".join(sorted(actual_names - expected_names)) or "none"
        raise BuildIdentityError(
            "matched module set contains binaries absent from the build identity "
            f"(unrecorded: {unrecorded})"
        )
    for name, path in module_elfs.items():
        require_artifact(path, expected[name].symbols, f"module ELF {name!r}")


def installed_remote_path(title_id: str, relative_path: str) -> str:
    if TITLE_ID_RE.fullmatch(title_id) is None:
        raise BuildIdentityError("title_id must match [A-Z0-9]{9}")
    checked = validate_installed_path(relative_path)
    return f"ux0:/app/{title_id}/{checked}"


def _remote_artifact(
    ftp: ftplib.FTP,
    remote_path: str,
    expected: InstalledArtifactIdentity,
) -> ArtifactIdentity:
    try:
        advertised = ftp.size(remote_path)
    except ftplib.all_errors as exc:
        raise BuildIdentityError(
            f"cannot read installed artifact size for {expected.path!r}: {exc}"
        ) from exc
    if not isinstance(advertised, int) or advertised != expected.size_bytes:
        raise BuildIdentityError(
            f"installed artifact {expected.path!r} has size {advertised!r}; "
            f"expected {expected.size_bytes}"
        )
    digest = hashlib.sha256()
    transferred = 0

    def consume(block: bytes) -> None:
        nonlocal transferred
        transferred += len(block)
        if transferred > expected.size_bytes:
            raise BuildIdentityError(
                f"installed artifact {expected.path!r} exceeded its recorded size"
            )
        digest.update(block)

    try:
        ftp.retrbinary(f"RETR {remote_path}", consume, blocksize=FTP_BLOCK_SIZE)
    except BuildIdentityError:
        raise
    except ftplib.all_errors as exc:
        raise BuildIdentityError(
            f"cannot read installed artifact {expected.path!r}: {exc}"
        ) from exc
    actual = ArtifactIdentity(digest.hexdigest(), transferred)
    wanted = ArtifactIdentity(expected.sha256, expected.size_bytes)
    if actual != wanted:
        raise BuildIdentityError(
            f"installed artifact {expected.path!r} does not match the build identity "
            f"(expected {wanted.sha256}/{wanted.size_bytes}, "
            f"got {actual.sha256}/{actual.size_bytes})"
        )
    return actual


def verify_installed_identity(
    identity: BuildIdentity,
    host: str,
    *,
    port: int = 1337,
    timeout: float = 10.0,
    ftp_factory: Callable[[], ftplib.FTP] = ftplib.FTP,
) -> tuple[ArtifactIdentity, ...]:
    """Hash every recorded installed binary; never mutate the Vita."""

    if not 1 <= port <= 65535 or timeout <= 0:
        raise BuildIdentityError("FTP port and timeout must be positive and in range")
    records: Iterable[InstalledArtifactIdentity] = (
        identity.main_installed,
        *(module.installed for module in identity.modules),
    )
    ftp: ftplib.FTP | None = None
    try:
        ftp = ftp_factory()
        ftp.connect(host, port, timeout=timeout)
        ftp.login()
        ftp.voidcmd("TYPE I")
        return tuple(
            _remote_artifact(
                ftp,
                installed_remote_path(identity.title_id, record.path),
                record,
            )
            for record in records
        )
    except BuildIdentityError:
        raise
    except ftplib.all_errors as exc:
        raise BuildIdentityError(f"cannot verify installed build over FTP: {exc}") from exc
    finally:
        if ftp is not None:
            try:
                ftp.quit()
            except ftplib.all_errors:
                try:
                    ftp.close()
                except OSError:
                    pass
