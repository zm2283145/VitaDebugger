#!/usr/bin/env python3
"""Create a VitaDebugger build identity from one packaged VPK build."""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "deploy"))

import gdb_symbols as symbols  # noqa: E402
from host.vitadevdeploy.errors import VpkValidationError  # noqa: E402
from host.vitadevdeploy.vpk import inspect_vpk  # noqa: E402
from uvdb_build_identity import (  # noqa: E402
    ArtifactIdentity,
    BuildIdentity,
    BuildIdentityError,
    InstalledArtifactIdentity,
    MODULE_NAME_RE,
    ModuleIdentity,
    artifact_identity,
    identity_to_json,
    parse_build_identity,
    validate_installed_path,
)


def parse_mappings(values: Sequence[str], label: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            raise BuildIdentityError(f"{label} requires NAME=VALUE, got {value!r}")
        name, selected = value.split("=", 1)
        if (
            MODULE_NAME_RE.fullmatch(name) is None
            or not selected
            or name in result
        ):
            raise BuildIdentityError(f"invalid or duplicate {label} mapping {value!r}")
        result[name] = selected
    return result


def _vpk_entry_identity(
    vpk_path: Path,
    archive: zipfile.ZipFile,
    entries: dict[str, object],
    relative_path: str,
) -> InstalledArtifactIdentity:
    checked_path = validate_installed_path(relative_path)
    entry = entries.get(checked_path.casefold())
    if entry is None or getattr(entry, "is_directory"):
        raise BuildIdentityError(f"VPK does not contain required file {checked_path!r}")
    expected_size = getattr(entry, "size")
    archive_name = getattr(entry, "archive_name")
    digest = hashlib.sha256()
    copied = 0
    try:
        with archive.open(archive_name, "r") as source:
            while True:
                block = source.read(1024 * 1024)
                if not block:
                    break
                copied += len(block)
                if copied > expected_size:
                    raise BuildIdentityError(
                        f"VPK member {checked_path!r} exceeded its declared size"
                    )
                digest.update(block)
    except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
        raise BuildIdentityError(
            f"cannot hash VPK member {checked_path!r} from {vpk_path}: {exc}"
        ) from exc
    if copied != expected_size:
        raise BuildIdentityError(
            f"VPK member {checked_path!r} changed size while it was read"
        )
    return InstalledArtifactIdentity(checked_path, digest.hexdigest(), copied)


def create_build_identity(
    vpk_path: Path | str,
    main_elf: Path | str,
    module_elfs: dict[str, Path | str],
    module_installed_paths: dict[str, str],
    *,
    expected_title_id: str | None = None,
) -> BuildIdentity:
    """Bind an exact VPK to its retained unstripped symbol files."""

    vpk = Path(vpk_path).resolve()
    main = Path(main_elf).resolve()
    if set(module_elfs) != set(module_installed_paths):
        missing_files = ", ".join(
            sorted(set(module_elfs) - set(module_installed_paths))
        ) or "none"
        missing_elfs = ", ".join(
            sorted(set(module_installed_paths) - set(module_elfs))
        ) or "none"
        raise BuildIdentityError(
            "module ELF and installed-path mappings must name the same modules "
            f"(missing installed paths: {missing_files}; missing ELFs: {missing_elfs})"
        )
    if len(module_elfs) > 256:
        raise BuildIdentityError("at most 256 module identities are supported")

    # Reject stripped, converted, or non-ARM files before writing a receipt.
    symbols.read_elf(main)
    resolved_module_paths: dict[str, Path] = {}
    for name, path_value in module_elfs.items():
        if MODULE_NAME_RE.fullmatch(name) is None:
            raise BuildIdentityError(f"invalid runtime module name {name!r}")
        path = Path(path_value).resolve()
        image = symbols.read_elf(path)
        if image.module_name is not None and image.module_name != name:
            raise BuildIdentityError(
                f"module ELF {path} embeds {image.module_name!r}, not {name!r}"
            )
        resolved_module_paths[name] = path
    all_symbol_paths = {main, *resolved_module_paths.values()}
    if len(all_symbol_paths) != 1 + len(resolved_module_paths):
        raise BuildIdentityError("one unstripped ELF cannot identify multiple binaries")

    vpk_identity = artifact_identity(vpk)
    try:
        inspection = inspect_vpk(vpk)
    except (VpkValidationError, OSError) as exc:
        raise BuildIdentityError(f"cannot validate VPK: {exc}") from exc
    if expected_title_id is not None and inspection.title_id != expected_title_id:
        raise BuildIdentityError(
            f"VPK title ID {inspection.title_id!r} does not match "
            f"{expected_title_id!r}"
        )
    entries = {entry.collision_key: entry for entry in inspection.entries}
    try:
        with zipfile.ZipFile(vpk, "r") as archive:
            main_installed = _vpk_entry_identity(vpk, archive, entries, "eboot.bin")
            modules = tuple(
                ModuleIdentity(
                    name,
                    artifact_identity(resolved_module_paths[name]),
                    _vpk_entry_identity(
                        vpk,
                        archive,
                        entries,
                        module_installed_paths[name],
                    ),
                )
                for name in sorted(module_elfs)
            )
    except (OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
        raise BuildIdentityError(f"cannot read VPK {vpk}: {exc}") from exc
    if artifact_identity(vpk) != vpk_identity:
        raise BuildIdentityError("VPK changed while its build identity was created")
    return BuildIdentity(
        inspection.title_id,
        vpk_identity,
        artifact_identity(main),
        main_installed,
        modules,
    )


def write_build_identity(
    identity: BuildIdentity,
    output: Path | str,
    *,
    force: bool = False,
) -> Path:
    target = Path(output).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and not force:
        raise BuildIdentityError(f"refusing to overwrite existing identity: {target}")
    text = identity_to_json(identity)
    # Validate our serialized form before publishing it.
    if parse_build_identity(text) != identity:
        raise BuildIdentityError("internal identity serialization mismatch")
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{target.name}.",
            suffix=".part",
            dir=target.parent,
            delete=False,
        ) as handle:
            temporary_name = handle.name
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, target)
        temporary_name = None
    except OSError as exc:
        raise BuildIdentityError(f"cannot write build identity {target}: {exc}") from exc
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except OSError:
                pass
    return target


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Create a versioned receipt binding one VPK to its unstripped "
            "main and user-module ELFs"
        )
    )
    parser.add_argument("--vpk", required=True, type=Path)
    parser.add_argument("--main-elf", required=True, type=Path)
    parser.add_argument(
        "--module", action="append", default=[], metavar="NAME=ELF",
        help="bind a runtime user-module name to its unstripped ELF",
    )
    parser.add_argument(
        "--module-installed", action="append", default=[], metavar="NAME=VPK_PATH",
        help="bind that runtime module to its packaged VPK-relative binary path",
    )
    parser.add_argument(
        "--title-id", help="optional expected VPK TITLE_ID ([A-Z0-9]{9})",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--force", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    module_elfs = {
        name: Path(path)
        for name, path in parse_mappings(args.module, "--module").items()
    }
    installed = parse_mappings(args.module_installed, "--module-installed")
    identity = create_build_identity(
        args.vpk,
        args.main_elf,
        module_elfs,
        installed,
        expected_title_id=args.title_id,
    )
    output = write_build_identity(identity, args.output, force=args.force)
    print(
        f"Wrote {output}: title={identity.title_id}, "
        f"modules={len(identity.modules)}, vpk_sha256={identity.vpk.sha256}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BuildIdentityError, symbols.SymbolError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
