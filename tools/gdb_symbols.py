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
import xml.etree.ElementTree as ET
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence


MAX_RSP_PACKET = 1024 * 1024
MAX_LIBRARY_XML = 1024 * 1024
MAX_LIBRARY_CHUNKS = 1024
MAX_MODULES = 256
MAX_SEGMENTS = 4
MAX_ELF_SEGMENTS = 4
MAX_ELF_SECTIONS = 4096
MAX_STRING_TABLE = 1024 * 1024
MAX_SCAN_FILES = 4096
SCAN_SUFFIXES = {".elf", ".velf", ".suprx", ".skprx"}

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
    for node in root:
        if node.tag != "library" or set(node.attrib) != {"name"}:
            raise ProtocolError("unexpected element or attributes in library list")
        name = node.attrib["name"]
        if not name or len(name) > 255 or any(ord(char) < 0x20 for char in name):
            raise ProtocolError("invalid runtime module name")
        if any(ord(char) > 0x7E for char in name):
            raise ProtocolError("runtime module name is not printable ASCII")
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
) -> tuple[tuple[ModuleMatch, ...], tuple[RuntimeModule, ...]]:
    runtime_names = {module.name for module in snapshot.modules}
    unknown_explicit = sorted(set(explicit) - runtime_names)
    if unknown_explicit:
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


class RspClient:
    def __init__(self, host: str, port: int, timeout: float):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.socket.settimeout(timeout)
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


def query_target(host: str, port: int, timeout: float) -> TargetSnapshot:
    client = RspClient(host, port, timeout)
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
    parser.add_argument("--output", type=Path, default=Path("uvdb-symbols.gdb"))
    parser.add_argument(
        "--gdb", type=Path,
        help="launch this GDB with the generated script immediately after the snapshot",
    )
    return parser


def run(args: argparse.Namespace) -> int:
    main_image = read_elf(args.main_elf)
    roots = args.search_root or [main_image.path.parent]
    images = scan_elfs(roots)
    explicit = parse_explicit(args.module)
    snapshot = query_target(args.host, args.port, args.timeout)
    main_module = reconcile_main(main_image, snapshot, args.main_module)
    matches, unmatched = match_modules(
        snapshot, main_module, images, explicit, args.allow_stem_match
    )
    matched_names = {match.runtime.name for match in matches}
    missing_required = sorted(set(args.require_module) - matched_names)
    if missing_required:
        raise SymbolError(
            "required runtime modules were not matched: " + ", ".join(missing_required)
        )
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
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
    output.write_text(script, encoding="utf-8", newline="\n")
    print(
        f"Wrote {output}: main={main_module.name!r}, "
        f"matched={len(matches)}, unmatched={len(unmatched)}, mode={args.mode}"
    )
    for module in unmatched:
        print(f"  no symbols: {module.name}")
    if args.gdb:
        command = [str(args.gdb.resolve()), "-x", str(output)]
        return subprocess.run(command, check=False).returncode
    return 0


def main() -> int:
    args = build_argument_parser().parse_args()
    if not 1 <= args.port <= 65535 or args.timeout <= 0:
        raise SymbolError("port and timeout must be positive and in range")
    if not SAFE_HOST.fullmatch(args.host):
        raise SymbolError("host must be a plain IPv4 address or DNS hostname")
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SymbolError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
