#!/usr/bin/env python3
"""Create an ASLR-correct GDB symbol script from a live VitaDebugger stop.

The tool deliberately uses only Python's standard library.  It takes one
bounded snapshot of qOffsets and qXfer:libraries:read, detaches cleanly, then
matches only unambiguous, unstripped ARM ELF files.  Run it again after every
application relaunch; addresses from an earlier process are never reusable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence

import uvdb_build_identity as build_identity


MAX_RSP_PACKET = 1024 * 1024
MAX_LIBRARY_XML = 1024 * 1024
MAX_LIBRARY_CHUNKS = 1024
MAX_MODULES = 256
MAX_SEGMENTS = 4
MAX_ELF_SEGMENTS = 4
MAX_ELF_SECTIONS = 4096
MAX_STRING_TABLE = 1024 * 1024
MAX_SCAN_FILES = 4096
MAX_SYMBOL_STATE = 1024 * 1024
MAX_CONNECT_WAIT = 60.0
CONNECT_RETRY_DELAY = 0.1
SCAN_SUFFIXES = {".elf", ".velf", ".suprx", ".skprx"}
SYMBOL_STATE_FORMAT = "VITADEBUGGER-SYMBOL-STATE-1"

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
EM_ARM = 40
ELF_TYPES = {2, 3, 0xFE04}
GDB_ELF_TYPES = {2, 3}
IDENTITY_SECTIONS = {
    ".symtab", ".strtab", ".debug_info", ".debug_abbrev",
    ".debug_line", ".debug_str",
}

HEX_ADDRESS = re.compile(r"(?:0x)?([0-9a-fA-F]{1,8})\Z")
SAFE_SECTION_NAME = re.compile(r"[A-Za-z0-9_.-]+\Z")
SAFE_HOST = re.compile(r"[A-Za-z0-9.-]+\Z")
SAFE_MODULE_FILENAME = re.compile(r"[A-Za-z0-9_.+-]{1,64}\Z")
SAFE_GDB_ARGUMENTS = {
    "-q", "--quiet", "-batch", "--batch", "-nh", "--nh",
    "--return-child-result",
}
SAFE_GDB_INTERPRETER = re.compile(r"--interpreter=mi(?:[23])?\Z")


class SymbolError(RuntimeError):
    """A fail-closed symbol workflow error."""


class ProtocolError(SymbolError):
    """Malformed or inconsistent RSP data."""


class ElfError(SymbolError):
    """A local file is not a usable unstripped ARM ELF."""


@dataclass(frozen=True)
class LoadSegment:
    vaddr: int
    memsz: int
    filesz: int
    flags: int


@dataclass(frozen=True)
class ElfSection:
    name: str
    address: int
    size: int
    flags: int
    section_type: int
    segment_index: int | None


@dataclass(frozen=True)
class ElfImage:
    path: Path
    elf_type: int
    module_name: str | None
    segments: tuple[LoadSegment, ...]
    sections: tuple[ElfSection, ...]
    identity_digest: str

    @property
    def allocated_sections(self) -> tuple[ElfSection, ...]:
        return tuple(
            section
            for section in self.sections
            if section.flags & SHF_ALLOC and section.size
        )

    def loaded_section_addresses(self, live_segments: Sequence[int]) -> dict[str, int]:
        if len(live_segments) != len(self.segments):
            raise SymbolError(
                f"{self.path}: local PT_LOAD count {len(self.segments)} does not "
                f"match target count {len(live_segments)}"
            )
        result: dict[str, int] = {}
        for section in self.allocated_sections:
            if section.segment_index is None:
                raise SymbolError(
                    f"{self.path}: allocated section {section.name!r} is not "
                    "contained in a PT_LOAD segment"
                )
            if not SAFE_SECTION_NAME.fullmatch(section.name):
                raise SymbolError(
                    f"{self.path}: section name {section.name!r} is unsafe for "
                    "a generated GDB command"
                )
            if section.name in result:
                raise SymbolError(
                    f"{self.path}: duplicate allocated section {section.name!r}"
                )
            segment = self.segments[section.segment_index]
            result[section.name] = (
                live_segments[section.segment_index]
                + section.address
                - segment.vaddr
            )
        if ".text" not in result:
            raise SymbolError(f"{self.path}: no non-empty allocated .text section")
        return result


@dataclass(frozen=True)
class RuntimeModule:
    name: str
    segments: tuple[int, ...]


@dataclass(frozen=True)
class OffsetReply:
    style: str
    text: int
    data: int | None


@dataclass(frozen=True)
class TargetSnapshot:
    offsets: OffsetReply
    modules: tuple[RuntimeModule, ...]


@dataclass(frozen=True)
class ModuleMatch:
    runtime: RuntimeModule
    image: ElfImage
    reason: str


@dataclass(frozen=True)
class ModuleChanges:
    added: tuple[str, ...]
    removed: tuple[str, ...]
    rebased: tuple[str, ...]
    symbol_changes: tuple[str, ...]
    build_changed: bool


def _checked_range(offset: int, size: int, file_size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise ElfError(f"{label} lies outside the file")


def _read_at(handle, offset: int, size: int, file_size: int, label: str) -> bytes:
    _checked_range(offset, size, file_size, label)
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise ElfError(f"short read while reading {label}")
    return data


def _string_from_table(table: bytes, offset: int, label: str) -> str:
    if offset >= len(table):
        raise ElfError(f"{label} name offset is outside the string table")
    end = table.find(b"\0", offset)
    if end < 0 or end - offset > 255:
        raise ElfError(f"{label} has an unterminated or overlong name")
    try:
        return table[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ElfError(f"{label} name is not ASCII") from exc


def _read_elf(path: Path | str) -> ElfImage:
    """Parse the small ELF subset needed to relocate debug sections."""

    source = Path(path).resolve()
    try:
        file_size = source.stat().st_size
        handle = source.open("rb")
    except OSError as exc:
        raise ElfError(f"cannot open {source}: {exc}") from exc

    with handle:
        header_data = _read_at(handle, 0, 52, file_size, "ELF header")
        fields = struct.unpack("<16sHHIIIIIHHHHHH", header_data)
        ident = fields[0]
        if ident[:4] != b"\x7fELF":
            raise ElfError(f"{source}: not an ELF file (a SELF/SUPRX is not a symbol file)")
        if ident[4] != 1 or ident[5] != 1 or ident[6] != 1:
            raise ElfError(f"{source}: expected ELF32 little-endian version 1")
        elf_type, machine = fields[1], fields[2]
        if elf_type not in ELF_TYPES or machine != EM_ARM:
            raise ElfError(f"{source}: expected an ARM executable/shared/Vita ELF")
        phoff, shoff = fields[5], fields[6]
        ehsize, phentsize, phnum = fields[8], fields[9], fields[10]
        shentsize, shnum, shstrndx = fields[11], fields[12], fields[13]
        if ehsize != 52 or phentsize != 32 or shentsize != 40:
            raise ElfError(f"{source}: unsupported ELF header table sizes")
        if not 1 <= phnum <= 64 or not 1 <= shnum <= MAX_ELF_SECTIONS:
            raise ElfError(f"{source}: unreasonable ELF table counts")
        if shstrndx == 0 or shstrndx >= shnum:
            raise ElfError(f"{source}: invalid section-name string table index")

        ph_data = _read_at(handle, phoff, phnum * phentsize, file_size, "program headers")
        segments: list[LoadSegment] = []
        for index in range(phnum):
            values = struct.unpack_from("<IIIIIIII", ph_data, index * 32)
            p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = values
            if p_type != PT_LOAD or not p_memsz:
                continue
            if p_filesz > p_memsz or p_vaddr + p_memsz > 0x100000000:
                raise ElfError(f"{source}: invalid PT_LOAD sizes")
            _checked_range(p_offset, p_filesz, file_size, "PT_LOAD contents")
            segments.append(LoadSegment(p_vaddr, p_memsz, p_filesz, p_flags))
        if not 1 <= len(segments) <= MAX_ELF_SEGMENTS:
            raise ElfError(
                f"{source}: expected 1-{MAX_ELF_SEGMENTS} non-empty PT_LOAD segments"
            )

        sh_data = _read_at(handle, shoff, shnum * shentsize, file_size, "section headers")
        raw_sections = [
            struct.unpack_from("<IIIIIIIIII", sh_data, index * 40)
            for index in range(shnum)
        ]
        string_header = raw_sections[shstrndx]
        if string_header[1] == SHT_NOBITS or string_header[5] > MAX_STRING_TABLE:
            raise ElfError(f"{source}: invalid section-name string table")
        shstr = _read_at(
            handle, string_header[4], string_header[5], file_size,
            "section-name string table",
        )

        sections: list[ElfSection] = []
        has_symtab = False
        embedded_name: str | None = None
        identity = hashlib.sha256()
        identity_count = 0
        for index, raw in enumerate(raw_sections):
            name_offset, section_type, flags, address, offset, size = raw[:6]
            name = _string_from_table(shstr, name_offset, f"section {index}")
            if section_type != SHT_NOBITS and size:
                _checked_range(offset, size, file_size, f"section {name!r}")
            if section_type == SHT_SYMTAB and size and raw[9] >= 16:
                has_symtab = True
            if name in IDENTITY_SECTIONS and section_type != SHT_NOBITS and size:
                identity.update(struct.pack("<I", len(name)))
                identity.update(name.encode("ascii"))
                identity.update(struct.pack("<I", size))
                remaining = size
                position = offset
                while remaining:
                    count = min(1024 * 1024, remaining)
                    identity.update(
                        _read_at(handle, position, count, file_size, f"section {name!r}")
                    )
                    position += count
                    remaining -= count
                identity_count += 1

            segment_index: int | None = None
            if flags & SHF_ALLOC and size:
                end = address + size
                if end > 0x100000000:
                    raise ElfError(f"{source}: section {name!r} wraps the address space")
                owners = [
                    segment_index
                    for segment_index, segment in enumerate(segments)
                    if segment.vaddr <= address
                    and end <= segment.vaddr + segment.memsz
                ]
                if len(owners) != 1:
                    raise ElfError(
                        f"{source}: allocated section {name!r} maps to "
                        f"{len(owners)} PT_LOAD segments"
                    )
                segment_index = owners[0]

            sections.append(
                ElfSection(name, address, size, flags, section_type, segment_index)
            )
            if name == ".sceModuleInfo.rodata" and size >= 32:
                prefix = _read_at(handle, offset, 32, file_size, "Vita module info")
                raw_name = prefix[4:31].split(b"\0", 1)[0]
                if raw_name:
                    try:
                        decoded = raw_name.decode("ascii")
                    except UnicodeDecodeError as exc:
                        raise ElfError(f"{source}: non-ASCII embedded module name") from exc
                    if any(ord(char) < 0x20 or ord(char) > 0x7E for char in decoded):
                        raise ElfError(f"{source}: invalid embedded module name")
                    embedded_name = decoded

        if not has_symtab:
            raise ElfError(f"{source}: no usable .symtab; provide the original unstripped ELF")
        if not identity_count:
            raise ElfError(f"{source}: no symbol/debug identity sections")
        return ElfImage(
            source, elf_type, embedded_name, tuple(segments), tuple(sections),
            identity.hexdigest(),
        )


def read_elf(path: Path | str) -> ElfImage:
    image = _read_elf(path)
    if image.elf_type not in GDB_ELF_TYPES:
        raise ElfError(
            f"{image.path}: Vita-converted ET_SCE_RELEXEC/Vita ELF is metadata, "
            "not a safe GDB symbol file; use its original linked .elf"
        )
    return image


def parse_address(text: str, label: str, *, allow_zero: bool = False) -> int:
    match = HEX_ADDRESS.fullmatch(text)
    if not match:
        raise ProtocolError(f"invalid {label} address {text!r}")
    value = int(match.group(1), 16)
    if not value and not allow_zero:
        raise ProtocolError(f"zero {label} address")
    return value


def parse_qoffsets(payload: bytes | str) -> OffsetReply:
    try:
        text = payload.decode("ascii") if isinstance(payload, bytes) else payload
    except UnicodeDecodeError as exc:
        raise ProtocolError("qOffsets reply is not ASCII") from exc
    fields: dict[str, int] = {}
    for item in text.split(";"):
        if not item or "=" not in item:
            raise ProtocolError(f"malformed qOffsets reply {text!r}")
        name, value_text = item.split("=", 1)
        if name in fields:
            raise ProtocolError(f"duplicate qOffsets field {name!r}")
        fields[name] = parse_address(
            value_text, f"qOffsets {name}", allow_zero=True
        )
    names = set(fields)
    if names <= {"TextSeg", "DataSeg"} and "TextSeg" in fields:
        if fields["TextSeg"] == 0 or fields.get("DataSeg") == 0:
            raise ProtocolError("qOffsets segment start addresses must be nonzero")
        return OffsetReply("segments", fields["TextSeg"], fields.get("DataSeg"))
    if names <= {"Text", "Data", "Bss"} and {"Text", "Data"} <= names:
        return OffsetReply("offsets", fields["Text"], fields["Data"])
    raise ProtocolError(f"unsupported or mixed qOffsets reply {text!r}")


def parse_library_xml(data: bytes) -> tuple[RuntimeModule, ...]:
    if not data or len(data) > MAX_LIBRARY_XML:
        raise ProtocolError("library XML is empty or exceeds the configured limit")
    upper = data.upper()
    if b"<!DOCTYPE" in upper or b"<!ENTITY" in upper:
        raise ProtocolError("DTD/entity declarations are forbidden in library XML")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        raise ProtocolError(f"malformed library XML: {exc}") from exc
    if root.tag != "library-list" or root.attrib != {"version": "1.0"}:
        raise ProtocolError("unexpected library-list root or attributes")
    if root.text and root.text.strip():
        raise ProtocolError("unexpected text in library-list root")
    if len(root) > MAX_MODULES:
        raise ProtocolError("library list contains too many modules")
    modules: list[RuntimeModule] = []
    module_names: set[str] = set()
    for node in root:
        if node.tag != "library" or set(node.attrib) != {"name"}:
            raise ProtocolError("unexpected element or attributes in library list")
        name = node.attrib["name"]
        if not name or len(name) > 255 or any(ord(char) < 0x20 for char in name):
            raise ProtocolError("invalid runtime module name")
        if any(ord(char) > 0x7E for char in name):
            raise ProtocolError("runtime module name is not printable ASCII")
        if name in module_names:
            raise ProtocolError(f"duplicate runtime module name {name!r}")
        module_names.add(name)
        if node.text and node.text.strip():
            raise ProtocolError(f"module {name!r} contains unexpected text")
        if node.tail and node.tail.strip():
            raise ProtocolError(f"module {name!r} has unexpected trailing text")
        if not 1 <= len(node) <= MAX_SEGMENTS:
            raise ProtocolError(f"module {name!r} has an invalid segment count")
        segments: list[int] = []
        for segment in node:
            if segment.tag != "segment" or set(segment.attrib) != {"address"}:
                raise ProtocolError(f"module {name!r} contains an invalid segment")
            if list(segment):
                raise ProtocolError(f"module {name!r} segment must be empty")
            if segment.text and segment.text.strip():
                raise ProtocolError(f"module {name!r} segment contains text")
            if segment.tail and segment.tail.strip():
                raise ProtocolError(f"module {name!r} segment has trailing text")
            segments.append(parse_address(segment.attrib["address"], f"{name} segment"))
        if len(set(segments)) != len(segments):
            raise ProtocolError(f"module {name!r} contains duplicate segment addresses")
        modules.append(RuntimeModule(name, tuple(segments)))
    return tuple(modules)


def expected_main_segments(image: ElfImage, offsets: OffsetReply) -> tuple[int, ...]:
    link = image.segments
    if offsets.style == "offsets":
        assert offsets.data is not None
        result = [link[0].vaddr + offsets.text]
        result.extend(segment.vaddr + offsets.data for segment in link[1:])
    else:
        result = [offsets.text]
        if len(link) > 1:
            anchor_index = 1 if offsets.data is not None else 0
            anchor_runtime = offsets.data if offsets.data is not None else offsets.text
            anchor_link = link[anchor_index].vaddr
            result.extend(
                anchor_runtime + segment.vaddr - anchor_link
                for segment in link[1:]
            )
    if any(value < 0 or value > 0xFFFFFFFF for value in result):
        raise SymbolError("qOffsets relocation moves the main ELF outside ARM32")
    return tuple(result)


def reconcile_main(
    image: ElfImage,
    snapshot: TargetSnapshot,
    requested_name: str | None = None,
) -> RuntimeModule:
    expected = expected_main_segments(image, snapshot.offsets)
    candidates = [module for module in snapshot.modules if module.segments == expected]
    if requested_name is not None:
        candidates = [module for module in candidates if module.name == requested_name]
    if len(candidates) != 1:
        names = ", ".join(repr(module.name) for module in candidates) or "none"
        raise SymbolError(
            "qOffsets does not identify exactly one qXfer library module "
            f"with all expected PT_LOAD addresses (matches: {names})"
        )
    return candidates[0]


def scan_elfs(roots: Iterable[Path | str]) -> tuple[ElfImage, ...]:
    paths: list[Path] = []
    seen: set[Path] = set()
    for root_value in roots:
        root = Path(root_value).resolve()
        if not root.is_dir():
            raise SymbolError(f"symbol search root is not a directory: {root}")
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.casefold() not in SCAN_SUFFIXES:
                continue
            resolved = path.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            paths.append(resolved)
            if len(paths) > MAX_SCAN_FILES:
                raise SymbolError(
                    f"symbol scan exceeds the {MAX_SCAN_FILES}-file safety limit"
                )
    parsed: list[ElfImage] = []
    for path in sorted(paths, key=lambda item: str(item).casefold()):
        try:
            parsed.append(_read_elf(path))
        except ElfError:
            # Packed SELF/SUPRX files and stripped release files are expected in
            # build trees. They cannot safely provide symbols and are ignored.
            continue
    symbol_images = [image for image in parsed if image.elf_type in GDB_ELF_TYPES]
    metadata_images = [image for image in parsed if image.elf_type not in GDB_ELF_TYPES]
    result: list[ElfImage] = []
    for image in symbol_images:
        if image.module_name is not None:
            result.append(image)
            continue
        sidecars = [
            metadata
            for metadata in metadata_images
            if metadata.module_name is not None
            and metadata.path.parent == image.path.parent
            and metadata.path.stem == image.path.stem
            and metadata.identity_digest == image.identity_digest
            and len(metadata.segments) == len(image.segments)
            and all(
                left.vaddr == right.vaddr
                for left, right in zip(metadata.segments, image.segments)
            )
        ]
        if len(sidecars) > 1:
            options = ", ".join(str(sidecar.path) for sidecar in sidecars)
            raise SymbolError(
                f"ambiguous Vita metadata sidecars for {image.path}: {options}"
            )
        if sidecars:
            image = replace(image, module_name=sidecars[0].module_name)
        result.append(image)
    return tuple(result)


def _compatible(image: ElfImage, module: RuntimeModule) -> bool:
    if len(image.segments) != len(module.segments):
        return False
    try:
        image.loaded_section_addresses(module.segments)
    except SymbolError:
        return False
    return True


def match_modules(
    snapshot: TargetSnapshot,
    main: RuntimeModule,
    images: Sequence[ElfImage],
    explicit: dict[str, ElfImage],
    allow_stem_match: bool = False,
    allow_absent_explicit: bool = False,
) -> tuple[tuple[ModuleMatch, ...], tuple[RuntimeModule, ...]]:
    runtime_names = {module.name for module in snapshot.modules}
    unknown_explicit = sorted(set(explicit) - runtime_names)
    if unknown_explicit and not allow_absent_explicit:
        raise SymbolError(
            "explicit mappings name modules not present in the live snapshot: "
            + ", ".join(unknown_explicit)
        )
    if main.name in explicit:
        raise SymbolError(
            f"{main.name!r} is the main module; select it with --main-elf, not --module"
        )
    matches: list[ModuleMatch] = []
    unmatched: list[RuntimeModule] = []
    used_paths: set[Path] = set()
    for module in snapshot.modules:
        if module is main:
            continue
        selected: ElfImage | None = None
        reason = ""
        if module.name in explicit:
            candidate = explicit[module.name]
            if not _compatible(candidate, module):
                raise SymbolError(
                    f"explicit mapping for {module.name!r} has an incompatible "
                    "PT_LOAD/section layout"
                )
            selected, reason = candidate, "explicit mapping"
        else:
            tiers: list[tuple[str, list[ElfImage]]] = [
                (
                    "embedded Vita module name",
                    [
                        image for image in images
                        if image.module_name is not None
                        and image.module_name == module.name
                        and _compatible(image, module)
                    ],
                ),
                (
                    "exact filename",
                    [
                        image for image in images
                        if image.module_name is None
                        and image.path.name == module.name
                        and _compatible(image, module)
                    ],
                ),
            ]
            if allow_stem_match:
                tiers.append(
                    (
                        "opt-in filename stem",
                        [
                            image for image in images
                            if image.module_name is None
                            and image.path.stem == Path(module.name).stem
                            and _compatible(image, module)
                        ],
                    )
                )
            for tier_reason, candidates in tiers:
                unique = {candidate.path: candidate for candidate in candidates}
                if len(unique) > 1:
                    options = ", ".join(str(path) for path in sorted(unique, key=str))
                    raise SymbolError(
                        f"ambiguous {tier_reason} match for {module.name!r}: {options}; "
                        "use --module NAME=PATH"
                    )
                if unique:
                    selected = next(iter(unique.values()))
                    reason = tier_reason
                    break
        if selected is None:
            unmatched.append(module)
            continue
        if selected.path in used_paths:
            raise SymbolError(
                f"local ELF {selected.path} would be reused for multiple runtime modules"
            )
        used_paths.add(selected.path)
        matches.append(ModuleMatch(module, selected, reason))
    return tuple(matches), tuple(unmatched)


def _rsp_frame(payload: bytes) -> bytes:
    if any(value in b"$#}*" for value in payload):
        raise ProtocolError("outgoing query unexpectedly requires RSP escaping")
    return b"$" + payload + f"#{sum(payload) & 0xff:02x}".encode("ascii")


def _connect_rsp_socket(
    host: str,
    port: int,
    timeout: float,
    connect_wait: float,
) -> socket.socket:
    """Return the first successfully connected socket within a bounded window.

    A successful TCP connection is the RSP session, not a readiness probe. Only
    failures raised by ``socket.create_connection`` are retryable; callers must
    never retry negotiation or any later protocol operation on another socket.
    """

    if not 0.0 <= connect_wait <= MAX_CONNECT_WAIT:
        raise SymbolError(
            f"connect wait must be between 0 and {MAX_CONNECT_WAIT:g} seconds"
        )
    deadline = time.monotonic() + connect_wait
    attempts = 0
    last_error: OSError | None = None
    while True:
        remaining = deadline - time.monotonic()
        if last_error is not None and remaining <= 0:
            raise SymbolError(
                f"GDB TCP endpoint {host}:{port} did not become ready within "
                f"{connect_wait:g} seconds after {attempts} attempts: {last_error}"
            ) from last_error
        attempt_timeout = timeout
        if connect_wait > 0:
            attempt_timeout = min(timeout, max(remaining, 0.001))
        attempts += 1
        try:
            return socket.create_connection(
                (host, port), timeout=attempt_timeout
            )
        except OSError as exc:
            if connect_wait == 0:
                raise
            last_error = exc
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SymbolError(
                    f"GDB TCP endpoint {host}:{port} did not become ready within "
                    f"{connect_wait:g} seconds after {attempts} attempts: {exc}"
                ) from exc
            time.sleep(min(CONNECT_RETRY_DELAY, remaining))


class RspClient:
    def __init__(
        self,
        host: str,
        port: int,
        timeout: float,
        connect_wait: float = 0.0,
    ):
        self.socket = _connect_rsp_socket(host, port, timeout, connect_wait)
        try:
            self.socket.settimeout(timeout)
        except Exception:
            self.socket.close()
            raise
        self.buffer = bytearray()
        self.ack_mode = True
        self.packet_size = 4096

    def close(self) -> None:
        self.socket.close()

    def _byte(self) -> int:
        if not self.buffer:
            block = self.socket.recv(4096)
            if not block:
                raise ProtocolError("debugger connection closed unexpectedly")
            self.buffer.extend(block)
        value = self.buffer[0]
        del self.buffer[0]
        return value

    def _expect_ack(self) -> None:
        while True:
            value = self._byte()
            if value == ord("+"):
                return
            if value == ord("-"):
                raise ProtocolError("stub rejected an RSP packet checksum")
            if value not in b"\r\n":
                raise ProtocolError(f"expected RSP acknowledgement, got 0x{value:02x}")

    def read_packet(self) -> bytes:
        while self._byte() != ord("$"):
            pass
        wire = bytearray()
        while True:
            value = self._byte()
            if value == ord("#"):
                break
            wire.append(value)
            if len(wire) > MAX_RSP_PACKET:
                raise ProtocolError("RSP packet exceeds the configured limit")
        checksum_bytes = bytes((self._byte(), self._byte()))
        try:
            checksum = int(checksum_bytes, 16)
        except ValueError as exc:
            raise ProtocolError("malformed RSP checksum") from exc
        if checksum != sum(wire) & 0xFF:
            if self.ack_mode:
                self.socket.sendall(b"-")
            raise ProtocolError("RSP checksum mismatch")
        if self.ack_mode:
            self.socket.sendall(b"+")

        decoded = bytearray()
        index = 0
        while index < len(wire):
            value = wire[index]
            if value == ord("}"):
                index += 1
                if index >= len(wire):
                    raise ProtocolError("truncated RSP escape")
                decoded.append(wire[index] ^ 0x20)
            elif value == ord("*"):
                raise ProtocolError("RSP run-length encoding is not accepted here")
            else:
                decoded.append(value)
            index += 1
        return bytes(decoded)

    def request(self, payload: bytes) -> bytes:
        self.socket.sendall(_rsp_frame(payload))
        if self.ack_mode:
            self._expect_ack()
        while True:
            reply = self.read_packet()
            is_console = (
                reply.startswith(b"O")
                and len(reply) % 2 == 1
                and all(value in b"0123456789abcdefABCDEF" for value in reply[1:])
            )
            if not is_console:
                return reply

    def negotiate(self) -> None:
        reply = self.request(b"qSupported:qXfer:libraries:read+;QStartNoAckMode+")
        try:
            features = reply.decode("ascii").split(";")
        except UnicodeDecodeError as exc:
            raise ProtocolError("qSupported reply is not ASCII") from exc
        if "qXfer:libraries:read+" not in features:
            raise ProtocolError("stub does not advertise qXfer:libraries:read+")
        packet = next((item for item in features if item.startswith("PacketSize=")), None)
        if packet:
            try:
                advertised = int(packet.split("=", 1)[1], 16)
            except ValueError as exc:
                raise ProtocolError("invalid qSupported PacketSize") from exc
            if advertised < 256:
                raise ProtocolError("qSupported PacketSize is too small")
            self.packet_size = min(advertised, MAX_RSP_PACKET)
        if "QStartNoAckMode+" in features:
            if self.request(b"QStartNoAckMode") != b"OK":
                raise ProtocolError("stub rejected QStartNoAckMode")
            self.ack_mode = False

    def read_library_xml(self) -> bytes:
        output = bytearray()
        chunk_size = min(0x1000, max(256, self.packet_size - 128))
        for _ in range(MAX_LIBRARY_CHUNKS):
            query = f"qXfer:libraries:read::{len(output):x},{chunk_size:x}".encode("ascii")
            reply = self.request(query)
            if not reply or reply[:1] not in (b"m", b"l"):
                raise ProtocolError("invalid qXfer:libraries:read chunk marker")
            chunk = reply[1:]
            if not chunk and reply[:1] == b"m":
                raise ProtocolError("non-final library XML chunk made no progress")
            if len(output) + len(chunk) > MAX_LIBRARY_XML:
                raise ProtocolError("library XML exceeds the configured limit")
            output.extend(chunk)
            if reply[:1] == b"l":
                return bytes(output)
        raise ProtocolError("library XML exceeds the configured chunk limit")


def query_target(
    host: str,
    port: int,
    timeout: float,
    connect_wait: float = 0.0,
) -> TargetSnapshot:
    client = RspClient(host, port, timeout, connect_wait)
    detached = False
    try:
        client.negotiate()
        stop = client.request(b"?")
        if not stop.startswith((b"T", b"S")):
            raise ProtocolError(f"expected initial stop reply, got {stop[:80]!r}")
        offsets_reply = client.request(b"qOffsets")
        if offsets_reply.startswith(b"E"):
            raise ProtocolError(f"stub rejected qOffsets: {offsets_reply!r}")
        offsets = parse_qoffsets(offsets_reply)
        modules = parse_library_xml(client.read_library_xml())
        if client.request(b"D") != b"OK":
            raise ProtocolError("stub rejected clean detach after symbol snapshot")
        detached = True
        return TargetSnapshot(offsets, modules)
    finally:
        # VitaDebugger enters its all-stop loop as soon as this RSP client is
        # accepted, before the explicit '?' query. Always make a bounded best-
        # effort detach so a malformed early reply does not rely solely on the
        # peer-loss watchdog to resume the application.
        if not detached:
            try:
                client.socket.settimeout(min(timeout, 1.0))
                client.request(b"D")
            except (OSError, SymbolError):
                pass
        client.close()


def _gdb_quote(value: str) -> str:
    if any(ord(char) < 0x20 for char in value):
        raise SymbolError("path contains a control character")
    return '"' + value.replace("\\", "/").replace('"', '\\"') + '"'


def _gdb_set_argument(value: str) -> str:
    """GDB `set ... path` consumes the full line; quotes become path bytes."""

    if any(ord(char) < 0x20 for char in value) or '"' in value:
        raise SymbolError("path cannot be represented safely in a GDB set command")
    return value.replace("\\", "/")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def _state_exact_keys(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise SymbolError(f"{label} must be a JSON object")
    keys = set(value)
    if keys != expected:
        raise SymbolError(f"{label} has unexpected or missing fields")
    return value


def _state_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise SymbolError(f"duplicate JSON field {key!r} in symbol state")
        result[key] = value
    return result


def validate_symbol_state(value: object) -> dict:
    """Validate a previous machine-readable refresh state fail closed."""

    state = _state_exact_keys(
        value,
        {"format", "build", "main", "offsets", "modules", "script_sha256"},
        "symbol state",
    )
    if state["format"] != SYMBOL_STATE_FORMAT:
        raise SymbolError(f"unsupported symbol state format {state['format']!r}")
    if not isinstance(state["script_sha256"], str) or not re.fullmatch(
        r"[0-9a-f]{64}", state["script_sha256"]
    ):
        raise SymbolError("symbol state has an invalid script SHA-256")
    build = _state_exact_keys(
        state["build"],
        {"verification", "title_id", "vpk_sha256", "identity_sha256"},
        "symbol state build",
    )
    if build["verification"] not in {"unverified", "local", "local+installed"}:
        raise SymbolError("symbol state has an invalid build verification mode")
    if build["verification"] == "unverified":
        if (
            build["title_id"] is not None
            or build["vpk_sha256"] is not None
            or build["identity_sha256"] is not None
        ):
            raise SymbolError("unverified symbol state must not claim a build identity")
    else:
        if (
            not isinstance(build["title_id"], str)
            or build_identity.TITLE_ID_RE.fullmatch(build["title_id"]) is None
            or not isinstance(build["vpk_sha256"], str)
            or build_identity.SHA256_RE.fullmatch(build["vpk_sha256"]) is None
            or not isinstance(build["identity_sha256"], str)
            or build_identity.SHA256_RE.fullmatch(build["identity_sha256"]) is None
        ):
            raise SymbolError("verified symbol state has invalid build identity fields")
    main = _state_exact_keys(
        state["main"], {"name", "elf_sha256"}, "symbol state main"
    )
    if (
        not isinstance(main["name"], str)
        or not main["name"]
        or len(main["name"]) > 255
        or any(ord(char) < 0x20 or ord(char) > 0x7E for char in main["name"])
        or not isinstance(main["elf_sha256"], str)
        or build_identity.SHA256_RE.fullmatch(main["elf_sha256"]) is None
    ):
        raise SymbolError("symbol state has invalid main-module fields")
    offsets = _state_exact_keys(
        state["offsets"], {"style", "text", "data"}, "symbol state offsets"
    )
    if offsets["style"] not in {"segments", "offsets"}:
        raise SymbolError("symbol state has invalid offset style")
    for name in ("text", "data"):
        item = offsets[name]
        if item is not None and (
            isinstance(item, bool) or not isinstance(item, int) or not 0 <= item <= 0xFFFFFFFF
        ):
            raise SymbolError(f"symbol state has invalid {name} offset")
    modules = state["modules"]
    if not isinstance(modules, list) or not 1 <= len(modules) <= MAX_MODULES:
        raise SymbolError("symbol state has an invalid module list")
    names: set[str] = set()
    for index, raw in enumerate(modules):
        module = _state_exact_keys(
            raw,
            {"name", "segments", "symbol_sha256", "match_reason"},
            f"symbol state module {index}",
        )
        name = module["name"]
        if (
            not isinstance(name, str)
            or not name
            or len(name) > 255
            or any(ord(char) < 0x20 or ord(char) > 0x7E for char in name)
            or name in names
        ):
            raise SymbolError("symbol state has an invalid or duplicate module name")
        names.add(name)
        segments = module["segments"]
        if (
            not isinstance(segments, list)
            or not 1 <= len(segments) <= MAX_SEGMENTS
            or any(
                isinstance(address, bool)
                or not isinstance(address, int)
                or not 0 < address <= 0xFFFFFFFF
                for address in segments
            )
            or len(set(segments)) != len(segments)
        ):
            raise SymbolError(f"symbol state module {name!r} has invalid segments")
        symbol_digest = module["symbol_sha256"]
        if symbol_digest is not None and (
            not isinstance(symbol_digest, str)
            or build_identity.SHA256_RE.fullmatch(symbol_digest) is None
        ):
            raise SymbolError(f"symbol state module {name!r} has an invalid symbol hash")
        reason = module["match_reason"]
        if reason is not None and (
            not isinstance(reason, str)
            or not reason
            or len(reason) > 64
            or any(ord(char) < 0x20 or ord(char) > 0x7E for char in reason)
        ):
            raise SymbolError(f"symbol state module {name!r} has an invalid match reason")
    return state


def load_symbol_state(path: Path, script_path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        size = path.stat().st_size
        if not path.is_file() or not 0 < size <= MAX_SYMBOL_STATE:
            raise SymbolError("symbol state is not a bounded regular file")
        data = path.read_bytes()
        if len(data) != size:
            raise SymbolError("symbol state changed while it was read")
        value = json.loads(
            data.decode("utf-8", "strict"), object_pairs_hook=_state_json_object
        )
    except SymbolError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SymbolError(f"cannot read previous symbol state: {exc}") from exc
    state = validate_symbol_state(value)
    if not script_path.is_file():
        raise SymbolError("previous symbol state exists but its script is missing")
    try:
        script_digest = sha256_file(script_path)
    except OSError as exc:
        raise SymbolError(f"cannot verify previous symbol script: {exc}") from exc
    if script_digest != state["script_sha256"]:
        raise SymbolError("previous symbol script does not match its state file")
    return state


def build_symbol_state(
    snapshot: TargetSnapshot,
    main_module: RuntimeModule,
    main_image: ElfImage,
    matches: Sequence[ModuleMatch],
    script: str,
    identity: build_identity.BuildIdentity | None,
    installed_verified: bool,
) -> dict:
    symbol_map = {
        match.runtime.name: (sha256_file(match.image.path), match.reason)
        for match in matches
    }
    modules = []
    for module in snapshot.modules:
        if module is main_module:
            symbol_digest, reason = sha256_file(main_image.path), "main executable"
        else:
            symbol_digest, reason = symbol_map.get(module.name, (None, None))
        modules.append(
            {
                "name": module.name,
                "segments": list(module.segments),
                "symbol_sha256": symbol_digest,
                "match_reason": reason,
            }
        )
    verification = (
        "local+installed" if identity is not None and installed_verified
        else "local" if identity is not None
        else "unverified"
    )
    state = {
        "format": SYMBOL_STATE_FORMAT,
        "build": {
            "verification": verification,
            "title_id": identity.title_id if identity is not None else None,
            "vpk_sha256": identity.vpk.sha256 if identity is not None else None,
            "identity_sha256": (
                hashlib.sha256(
                    build_identity.identity_to_json(identity).encode("utf-8")
                ).hexdigest()
                if identity is not None else None
            ),
        },
        "main": {
            "name": main_module.name,
            "elf_sha256": sha256_file(main_image.path),
        },
        "offsets": {
            "style": snapshot.offsets.style,
            "text": snapshot.offsets.text,
            "data": snapshot.offsets.data,
        },
        "modules": modules,
        "script_sha256": hashlib.sha256(script.encode("utf-8")).hexdigest(),
    }
    return validate_symbol_state(state)


def compare_symbol_states(previous: dict | None, current: dict) -> ModuleChanges:
    validate_symbol_state(current)
    if previous is None:
        return ModuleChanges(
            tuple(module["name"] for module in current["modules"]),
            (), (), (), False,
        )
    validate_symbol_state(previous)
    old = {module["name"]: module for module in previous["modules"]}
    new = {module["name"]: module for module in current["modules"]}
    common = set(old) & set(new)
    return ModuleChanges(
        tuple(sorted(set(new) - set(old))),
        tuple(sorted(set(old) - set(new))),
        tuple(sorted(
            name for name in common
            if old[name]["segments"] != new[name]["segments"]
        )),
        tuple(sorted(
            name for name in common
            if (
                old[name]["symbol_sha256"] != new[name]["symbol_sha256"]
                or old[name]["match_reason"] != new[name]["match_reason"]
            )
        )),
        previous["build"]["title_id"] != current["build"]["title_id"]
        or previous["build"]["vpk_sha256"] != current["build"]["vpk_sha256"]
        or previous["build"]["identity_sha256"] != current["build"]["identity_sha256"]
        or previous["main"]["elf_sha256"] != current["main"]["elf_sha256"],
    )


def _stage_text(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="\n",
        prefix=f".{path.name}.",
        suffix=".part",
        dir=path.parent,
        delete=False,
    )
    try:
        with handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        return Path(handle.name)
    except Exception:
        try:
            Path(handle.name).unlink()
        except OSError:
            pass
        raise


def _stage_bytes(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        mode="wb",
        prefix=f".{path.name}.",
        suffix=".part",
        dir=path.parent,
        delete=False,
    )
    try:
        with handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        return Path(handle.name)
    except Exception:
        try:
            Path(handle.name).unlink()
        except OSError:
            pass
        raise


def _restore_file(path: Path, previous: bytes | None) -> None:
    if previous is None:
        if path.exists():
            path.unlink()
        return
    staged = _stage_bytes(path, previous)
    os.replace(staged, path)


def publish_symbol_view(
    script_path: Path,
    script: str,
    state_path: Path,
    state: dict,
) -> bool:
    """Atomically stage a script/state pair and roll back partial commits."""

    script_path = script_path.resolve()
    state_path = state_path.resolve()
    if script_path == state_path:
        raise SymbolError("symbol script and state paths must differ")
    state_text = json.dumps(state, indent=2, sort_keys=True) + "\n"
    validate_symbol_state(json.loads(state_text))
    old_script = script_path.read_bytes() if script_path.exists() else None
    old_state = state_path.read_bytes() if state_path.exists() else None
    if (
        old_script == script.encode("utf-8")
        and old_state == state_text.encode("utf-8")
    ):
        return False
    staged_script: Path | None = None
    staged_state: Path | None = None
    script_committed = False
    state_committed = False
    try:
        staged_script = _stage_text(script_path, script)
        staged_state = _stage_text(state_path, state_text)
        os.replace(staged_script, script_path)
        staged_script = None
        script_committed = True
        os.replace(staged_state, state_path)
        staged_state = None
        state_committed = True
        return True
    except (OSError, UnicodeError) as exc:
        try:
            if script_committed:
                _restore_file(script_path, old_script)
            if state_committed:
                _restore_file(state_path, old_state)
        except (OSError, UnicodeError) as rollback_exc:
            raise SymbolError(
                "symbol view publication failed and rollback also failed: "
                f"{rollback_exc}"
            ) from exc
        raise SymbolError(f"symbol view publication failed safely: {exc}") from exc
    finally:
        for temporary in (staged_script, staged_state):
            if temporary is not None:
                try:
                    temporary.unlink()
                except OSError:
                    pass


def build_gdb_command(
    executable: Path,
    script: Path,
    arguments: Sequence[str],
    *,
    batch: bool,
) -> list[str]:
    if len(arguments) > 16:
        raise SymbolError("at most 16 GDB launch arguments are supported")
    checked: list[str] = []
    for argument in arguments:
        if (
            not isinstance(argument, str)
            or not argument
            or any(ord(char) < 0x20 or ord(char) == 0x7F for char in argument)
            or (
                argument not in SAFE_GDB_ARGUMENTS
                and SAFE_GDB_INTERPRETER.fullmatch(argument) is None
            )
        ):
            raise SymbolError(f"unsafe or unsupported GDB launch argument {argument!r}")
        checked.append(argument)
    if batch and "--batch" not in checked and "-batch" not in checked:
        checked.append("--batch")
    return [str(executable.resolve()), "-nx", *checked, "-x", str(script.resolve())]


def materialize_solib_cache(
    matches: Sequence[ModuleMatch], cache_root: Path
) -> Path:
    """Create an immutable, content-addressed library-name view for GDB."""

    entries: list[tuple[str, str, Path]] = []
    seen_names: set[str] = set()
    for match in matches:
        name = match.runtime.name
        windows_base = name.split(".", 1)[0].casefold()
        windows_reserved = (
            windows_base in {"con", "prn", "aux", "nul"}
            or re.fullmatch(r"(?:com|lpt)[1-9]", windows_base) is not None
        )
        if (
            not SAFE_MODULE_FILENAME.fullmatch(name)
            or name in {".", ".."}
            or name.endswith(".")
            or (os.name == "nt" and windows_reserved)
        ):
            raise SymbolError(
                f"runtime module name {name!r} cannot be materialized safely; "
                "use --mode explicit"
            )
        folded = name.casefold() if os.name == "nt" else name
        if folded in seen_names:
            raise SymbolError(f"runtime module filename collision for {name!r}")
        seen_names.add(folded)
        entries.append((name, sha256_file(match.image.path), match.image.path))
    identity = hashlib.sha256()
    for name, digest, _ in sorted(entries):
        identity.update(name.encode("ascii"))
        identity.update(b"\0")
        identity.update(digest.encode("ascii"))
        identity.update(b"\0")
    directory = cache_root.resolve() / identity.hexdigest()[:24]
    directory.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": 1,
        "modules": [
            {"name": name, "sha256": digest}
            for name, digest, _ in sorted(entries)
        ],
    }
    manifest_text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    manifest_path = directory / "manifest.json"
    if manifest_path.exists():
        if manifest_path.read_text(encoding="utf-8") != manifest_text:
            raise SymbolError(f"solib cache manifest collision at {manifest_path}")
    else:
        manifest_path.write_text(manifest_text, encoding="utf-8", newline="\n")
    for name, expected_digest, source in entries:
        destination = directory / name
        if destination.exists():
            if not destination.is_file() or sha256_file(destination) != expected_digest:
                raise SymbolError(f"solib cache entry is inconsistent: {destination}")
            continue
        try:
            shutil.copyfile(source, destination)
        except OSError as exc:
            raise SymbolError(f"cannot create solib cache entry {destination}: {exc}") from exc
        if sha256_file(destination) != expected_digest:
            raise SymbolError(f"solib cache copy verification failed: {destination}")
    return directory


def render_gdb_script(
    host: str,
    port: int,
    snapshot: TargetSnapshot,
    main_module: RuntimeModule,
    main_image: ElfImage,
    matches: Sequence[ModuleMatch],
    unmatched: Sequence[RuntimeModule],
    mode: str = "explicit",
    solib_directory: Path | None = None,
) -> str:
    directories = sorted(
        {main_image.path.parent, *(match.image.path.parent for match in matches)},
        key=lambda item: str(item).casefold(),
    )
    path_separator = ";" if os.name == "nt" else ":"
    solib_path = path_separator.join(path.as_posix() for path in directories)
    if mode not in {"explicit", "solib"}:
        raise SymbolError(f"unknown symbol script mode {mode!r}")
    if mode == "solib":
        if solib_directory is None:
            raise SymbolError("solib mode requires a materialized symbol directory")
        solib_path = solib_directory.resolve().as_posix()
    lines = [
        "# Generated by tools/gdb_symbols.py from one live VitaDebugger stop.",
        (
            "# Solib mode queries fresh ASLR addresses on every connection; rerun "
            "after deploying different binaries."
            if mode == "solib"
            else "# Explicit mode is one-launch-only; regenerate after every target relaunch."
        ),
        f"# Main runtime module: {main_module.name}",
        f"# qOffsets: style={snapshot.offsets.style} text=0x{snapshot.offsets.text:08x}"
        + (f" data=0x{snapshot.offsets.data:08x}" if snapshot.offsets.data is not None else ""),
        f"# Main ELF SHA-256: {sha256_file(main_image.path)}",
        "set pagination off",
        "set confirm off",
        f"set auto-solib-add {'on' if mode == 'solib' else 'off'}",
        f"set solib-search-path {_gdb_set_argument(solib_path)}",
        f"file {_gdb_quote(main_image.path.as_posix())}",
        f"target remote {host}:{port}",
        "# GDB applies the main executable's qOffsets during target connection.",
    ]
    if mode == "solib":
        # A nonexistent sysroot makes GDB fall through to the audited solib
        # search directory instead of accepting an unrelated host file first.
        missing_sysroot = solib_directory.resolve() / ".no-sysroot"
        lines.insert(
            lines.index(f"set solib-search-path {_gdb_set_argument(solib_path)}"),
            f"set sysroot {_gdb_set_argument(missing_sysroot.as_posix())}",
        )
    for match in matches:
        lines.append(
            f"# {match.runtime.name}: {match.reason}; SHA-256 "
            f"{sha256_file(match.image.path)}"
        )
        if mode == "explicit":
            addresses = match.image.loaded_section_addresses(match.runtime.segments)
            command = [
                "add-symbol-file",
                _gdb_quote(match.image.path.as_posix()),
                f"0x{addresses['.text']:08x}",
            ]
            for name, address in sorted(addresses.items()):
                if name == ".text":
                    continue
                command.extend(("-s", name, f"0x{address:08x}"))
            lines.append(" ".join(command))
    if mode == "solib" and matches:
        lines.append("sharedlibrary")
    for module in unmatched:
        addresses = ",".join(f"0x{address:08x}" for address in module.segments)
        lines.append(f"# UNMATCHED {module.name}: {addresses}")
    lines.extend(("", "# End of ASLR-specific symbol commands.", ""))
    return "\n".join(lines)


def parse_explicit(values: Sequence[str]) -> dict[str, ElfImage]:
    result: dict[str, ElfImage] = {}
    for value in values:
        if "=" not in value:
            raise SymbolError(f"--module requires NAME=PATH, got {value!r}")
        name, path_text = value.split("=", 1)
        if not name or not path_text or name in result:
            raise SymbolError(f"invalid or duplicate explicit module mapping {value!r}")
        result[name] = read_elf(path_text)
    return result


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Snapshot Vita ASLR module addresses and generate a GDB script"
    )
    parser.add_argument("--main-elf", required=True, type=Path)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--connect-wait",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help=(
            "wait up to SECONDS for a freshly launched GDB TCP endpoint "
            f"(0 disables retries; maximum {MAX_CONNECT_WAIT:g}); only failed "
            "connection attempts are retried, and the first successful socket "
            "is used for the snapshot"
        ),
    )
    parser.add_argument("--search-root", action="append", type=Path, default=[])
    parser.add_argument("--module", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--main-module", help="expected runtime main module name")
    parser.add_argument("--require-module", action="append", default=[])
    parser.add_argument(
        "--allow-stem-match", action="store_true",
        help="allow a unique foo.elf -> foo.suprx stem match (off by default)",
    )
    parser.add_argument(
        "--mode", choices=("solib", "explicit"), default="solib",
        help=(
            "solib builds an immutable name cache and refreshes ASLR on every "
            "GDB connection; explicit emits one-launch add-symbol-file commands"
        ),
    )
    parser.add_argument(
        "--solib-cache-root", type=Path,
        help="content-addressed cache root (default: .uvdb-solib beside --output)",
    )
    identity = parser.add_mutually_exclusive_group(required=True)
    identity.add_argument(
        "--build-identity", type=Path,
        help=(
            "versioned receipt created by gdb_build_identity.py; local VPK and "
            "ELF hashes must match before symbols are emitted"
        ),
    )
    identity.add_argument(
        "--allow-unverified-build", action="store_true",
        help=(
            "explicit compatibility mode without a build identity; never "
            "reported as an identity match"
        ),
    )
    parser.add_argument(
        "--vpk", type=Path,
        help="exact local VPK named by --build-identity",
    )
    parser.add_argument(
        "--verify-installed", action="store_true",
        help=(
            "read-only hash each recorded ux0:app binary through Vita Companion "
            "FTP; any unavailable or mismatched file fails closed"
        ),
    )
    parser.add_argument("--ftp-port", type=int, default=1337)
    parser.add_argument("--ftp-timeout", type=float, default=10.0)
    parser.add_argument("--output", type=Path, default=Path("uvdb-symbols.gdb"))
    parser.add_argument(
        "--state-file", type=Path,
        help=(
            "machine-readable module refresh state (default: OUTPUT.state.json); "
            "added, removed, and rebased modules are reported on the next run"
        ),
    )
    parser.add_argument(
        "--gdb", type=Path,
        help="launch this GDB with the generated script immediately after the snapshot",
    )
    parser.add_argument(
        "--gdb-arg", action="append", default=[],
        help=(
            "allowlisted noninteractive GDB option; use "
            "--gdb-arg=--interpreter=mi2 for an IDE/MI session"
        ),
    )
    parser.add_argument(
        "--gdb-batch", action="store_true",
        help="launch GDB noninteractively with --batch after refreshing symbols",
    )
    return parser


def run(args: argparse.Namespace) -> int:
    output = args.output.resolve()
    state_path = (
        args.state_file.resolve()
        if getattr(args, "state_file", None) is not None
        else output.with_name(output.name + ".state.json")
    )
    previous_state = load_symbol_state(state_path, output)
    requested_gdb = getattr(args, "gdb", None)
    requested_gdb_args = getattr(args, "gdb_arg", ())
    requested_gdb_batch = bool(getattr(args, "gdb_batch", False))
    if requested_gdb is None and (requested_gdb_args or requested_gdb_batch):
        raise SymbolError("--gdb-arg/--gdb-batch require --gdb")
    gdb_command = None
    if requested_gdb is not None:
        if not requested_gdb.resolve().is_file():
            raise SymbolError("--gdb must name an existing regular file")
        gdb_command = build_gdb_command(
            requested_gdb,
            output,
            requested_gdb_args,
            batch=requested_gdb_batch,
        )
    main_image = read_elf(args.main_elf)
    identity_path = getattr(args, "build_identity", None)
    allow_unverified = bool(getattr(args, "allow_unverified_build", False))
    verify_installed = bool(getattr(args, "verify_installed", False))
    if (
        (identity_path is None and not allow_unverified)
        or (identity_path is not None and allow_unverified)
    ):
        raise SymbolError(
            "select exactly one of --build-identity or --allow-unverified-build"
        )
    selected_identity = None
    installed_verified = False
    if identity_path is not None:
        vpk_path = getattr(args, "vpk", None)
        if vpk_path is None:
            raise SymbolError("--build-identity requires --vpk")
        try:
            selected_identity = build_identity.load_build_identity(identity_path)
            # Check the two core local artifacts before opening a debugger
            # connection or touching the target's run state.
            build_identity.require_artifact(vpk_path, selected_identity.vpk, "VPK")
            build_identity.require_artifact(
                main_image.path, selected_identity.main_symbols, "main ELF"
            )
        except build_identity.BuildIdentityError as exc:
            raise SymbolError(f"build identity check failed: {exc}") from exc
    elif verify_installed:
        raise SymbolError(
            "--verify-installed requires --build-identity; compatibility mode "
            "cannot attest the installed build"
        )
    roots = args.search_root or [main_image.path.parent]
    images = scan_elfs(roots)
    explicit = parse_explicit(args.module)
    if selected_identity is not None and explicit:
        try:
            # Identity-backed mappings may describe a module that has not
            # loaded yet or has just unloaded. Validate those dormant paths
            # now; a future refresh can use them without weakening typo/hash
            # checks merely because the module is temporarily absent.
            build_identity.verify_local_identity(
                selected_identity,
                args.vpk,
                main_image.path,
                {name: image.path for name, image in explicit.items()},
            )
        except build_identity.BuildIdentityError as exc:
            raise SymbolError(f"build identity check failed: {exc}") from exc
    if selected_identity is not None and verify_installed:
        try:
            build_identity.verify_installed_identity(
                selected_identity,
                args.host,
                port=getattr(args, "ftp_port", 1337),
                timeout=getattr(args, "ftp_timeout", 10.0),
            )
            installed_verified = True
        except build_identity.BuildIdentityError as exc:
            raise SymbolError(f"build identity check failed: {exc}") from exc
    snapshot = query_target(
        args.host,
        args.port,
        args.timeout,
        getattr(args, "connect_wait", 0.0),
    )
    main_module = reconcile_main(main_image, snapshot, args.main_module)
    matches, unmatched = match_modules(
        snapshot,
        main_module,
        images,
        explicit,
        args.allow_stem_match,
        allow_absent_explicit=selected_identity is not None,
    )
    matched_names = {match.runtime.name for match in matches}
    missing_required = sorted(set(args.require_module) - matched_names)
    if missing_required:
        raise SymbolError(
            "required runtime modules were not matched: " + ", ".join(missing_required)
        )
    if selected_identity is not None:
        try:
            build_identity.verify_local_identity(
                selected_identity,
                args.vpk,
                main_image.path,
                {match.runtime.name: match.image.path for match in matches},
            )
        except build_identity.BuildIdentityError as exc:
            raise SymbolError(f"build identity check failed: {exc}") from exc
    solib_directory = None
    if args.mode == "solib":
        cache_root = (
            args.solib_cache_root.resolve()
            if args.solib_cache_root
            else output.parent / ".uvdb-solib"
        )
        solib_directory = materialize_solib_cache(matches, cache_root)
    script = render_gdb_script(
        args.host, args.port, snapshot, main_module, main_image, matches, unmatched,
        args.mode, solib_directory,
    )
    if selected_identity is not None:
        verification = "local+installed" if installed_verified else "local-only"
        script = (
            f"# Build identity: {build_identity.IDENTITY_FORMAT}; "
            f"title={selected_identity.title_id}; verification={verification}\n"
            f"# VPK SHA-256: {selected_identity.vpk.sha256}\n"
            + script
        )
    else:
        script = "# Build identity: UNVERIFIED COMPATIBILITY MODE\n" + script
    state = build_symbol_state(
        snapshot,
        main_module,
        main_image,
        matches,
        script,
        selected_identity,
        installed_verified,
    )
    changes = compare_symbol_states(previous_state, state)
    changed = publish_symbol_view(output, script, state_path, state)
    print(
        f"Wrote {output}: main={main_module.name!r}, "
        f"matched={len(matches)}, unmatched={len(unmatched)}, mode={args.mode}, "
        f"identity={state['build']['verification']}, "
        f"updated={'yes' if changed else 'no'}"
    )
    print(
        f"Module refresh: added={','.join(changes.added) or '-'}; "
        f"removed={','.join(changes.removed) or '-'}; "
        f"rebased={','.join(changes.rebased) or '-'}; "
        f"symbol_changes={','.join(changes.symbol_changes) or '-'}; "
        f"build_changed={'yes' if changes.build_changed else 'no'}"
    )
    print(f"State: {state_path}")
    for module in unmatched:
        print(f"  no symbols: {module.name}")
    if gdb_command is not None:
        return subprocess.run(gdb_command, check=False, shell=False).returncode
    return 0


def main() -> int:
    args = build_argument_parser().parse_args()
    if not 1 <= args.port <= 65535 or args.timeout <= 0:
        raise SymbolError("port and timeout must be positive and in range")
    if not 0.0 <= args.connect_wait <= MAX_CONNECT_WAIT:
        raise SymbolError(
            f"connect wait must be between 0 and {MAX_CONNECT_WAIT:g} seconds"
        )
    if not 1 <= args.ftp_port <= 65535 or args.ftp_timeout <= 0:
        raise SymbolError("FTP port and timeout must be positive and in range")
    if not SAFE_HOST.fullmatch(args.host):
        raise SymbolError("host must be a plain IPv4 address or DNS hostname")
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SymbolError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
