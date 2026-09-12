"""Minimal, bounds-checked parser for PlayStation PARAM.SFO files."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from typing import Mapping

from .errors import SfoFormatError
from .paths import TITLE_ID_RE

_HEADER = struct.Struct("<4s4I")
_ENTRY = struct.Struct("<HHIII")
_MAGIC = b"\x00PSF"
_STRING_FORMAT = 0x0204
_INTEGER_FORMAT = 0x0404


@dataclass(frozen=True)
class SfoMetadata:
    title_id: str
    content_id: str | None
    values: Mapping[str, object]


def _checked_range(start: int, length: int, total: int, label: str) -> tuple[int, int]:
    if start < 0 or length < 0 or start > total or length > total - start:
        raise SfoFormatError(f"{label} lies outside param.sfo")
    return start, start + length


def parse_sfo(data: bytes) -> SfoMetadata:
    if len(data) < _HEADER.size:
        raise SfoFormatError("param.sfo is shorter than its header")
    magic, _version, key_offset, data_offset, count = _HEADER.unpack_from(data)
    if magic != _MAGIC:
        raise SfoFormatError("param.sfo has an invalid magic value")
    if count > 4096:
        raise SfoFormatError("param.sfo contains too many entries")

    index_end = _HEADER.size + count * _ENTRY.size
    if index_end > len(data):
        raise SfoFormatError("param.sfo index table is truncated")
    if key_offset < index_end or data_offset < key_offset or data_offset > len(data):
        raise SfoFormatError("param.sfo table offsets are invalid")

    values: dict[str, object] = {}
    for index in range(count):
        offset = _HEADER.size + index * _ENTRY.size
        key_rel, value_format, value_len, value_max_len, value_rel = _ENTRY.unpack_from(data, offset)
        if value_len > value_max_len:
            raise SfoFormatError("param.sfo value length exceeds its declared maximum")

        key_start = key_offset + key_rel
        if key_start < key_offset or key_start >= data_offset:
            raise SfoFormatError("param.sfo key offset is invalid")
        key_end = data.find(b"\x00", key_start, data_offset)
        if key_end < 0:
            raise SfoFormatError("param.sfo key is not NUL terminated")
        try:
            key = data[key_start:key_end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise SfoFormatError("param.sfo key is not ASCII") from exc
        if not key or key in values:
            raise SfoFormatError("param.sfo contains an empty or duplicate key")

        value_start = data_offset + value_rel
        _, value_end = _checked_range(value_start, value_len, len(data), f"value for {key}")
        _checked_range(value_start, value_max_len, len(data), f"value capacity for {key}")
        raw = data[value_start:value_end]
        if value_format == _STRING_FORMAT:
            raw = raw.split(b"\x00", 1)[0]
            try:
                value: object = raw.decode("utf-8", "strict")
            except UnicodeDecodeError as exc:
                raise SfoFormatError(f"string value for {key} is not valid UTF-8") from exc
        elif value_format == _INTEGER_FORMAT:
            if value_len != 4:
                raise SfoFormatError(f"integer value for {key} is not four bytes")
            value = struct.unpack_from("<I", raw)[0]
        else:
            value = raw
        values[key] = value

    title_id = values.get("TITLE_ID")
    if not isinstance(title_id, str) or not TITLE_ID_RE.fullmatch(title_id):
        raise SfoFormatError("TITLE_ID must be exactly nine uppercase ASCII letters or digits")
    content_id = values.get("CONTENT_ID")
    if content_id is not None and not isinstance(content_id, str):
        raise SfoFormatError("CONTENT_ID is not a string")
    if isinstance(content_id, str):
        try:
            encoded_content_id = content_id.encode("ascii", "strict")
        except UnicodeEncodeError as exc:
            raise SfoFormatError("CONTENT_ID must contain ASCII only") from exc
        if len(encoded_content_id) > 48 or not re.fullmatch(r"[ -~]*", content_id):
            raise SfoFormatError("CONTENT_ID must be at most 48 printable ASCII bytes")
        if not content_id:
            content_id = None
    return SfoMetadata(title_id=title_id, content_id=content_id, values=values)
