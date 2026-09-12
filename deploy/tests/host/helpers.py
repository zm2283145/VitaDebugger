from __future__ import annotations

import struct
import zipfile
from pathlib import Path


def make_sfo(title_id: str = "TEST00001", content_id: str | None = None) -> bytes:
    values = {"TITLE_ID": title_id}
    if content_id is not None:
        values["CONTENT_ID"] = content_id
    keys = bytearray()
    value_data = bytearray()
    entries: list[tuple[int, int, int, int, int]] = []
    for key, value in values.items():
        key_offset = len(keys)
        keys.extend(key.encode("ascii") + b"\x00")
        encoded = value.encode("utf-8") + b"\x00"
        value_offset = len(value_data)
        value_data.extend(encoded)
        entries.append((key_offset, 0x0204, len(encoded), len(encoded), value_offset))
    index_end = 20 + len(entries) * 16
    key_offset = index_end
    data_offset = key_offset + len(keys)
    header = struct.pack("<4s4I", b"\x00PSF", 0x00000101, key_offset, data_offset, len(entries))
    indexes = b"".join(struct.pack("<HHIII", *entry) for entry in entries)
    return header + indexes + keys + value_data


def make_vpk(path: Path, *, extra: dict[str, bytes] | None = None, title_id: str = "TEST00001") -> Path:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("eboot.bin", b"vita executable")
        archive.writestr("sce_sys/param.sfo", make_sfo(title_id))
        for name, data in (extra or {}).items():
            archive.writestr(name, data)
    return path


def make_head_template() -> bytes:
    template = bytearray(0x240)
    struct.pack_into(">I", template, 0x08, 0x120)
    struct.pack_into(">I", template, 0x10, 0x80)
    struct.pack_into(">I", template, 0xD0, 0x100)
    struct.pack_into(">I", template, 0xD4, 0x180)
    struct.pack_into(">I", template, 0xE8, 0x200)
    for index in range(len(template)):
        if template[index] == 0:
            template[index] = (index * 17 + 3) & 0xFF
    # Restore fields after filling the deterministic body.
    struct.pack_into(">I", template, 0x08, 0x120)
    struct.pack_into(">I", template, 0x10, 0x80)
    struct.pack_into(">I", template, 0xD0, 0x100)
    struct.pack_into(">I", template, 0xD4, 0x180)
    struct.pack_into(">I", template, 0xE8, 0x200)
    return bytes(template)
