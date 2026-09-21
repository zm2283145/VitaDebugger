"""VitaDebugger screen protocol v1 parsing and mock encoding."""

from __future__ import annotations

import dataclasses
import struct
import zlib


AUTH_MAGIC = b"VDSA"
FRAME_MAGIC = b"VDSC"
VERSION = 1
AUTH_SIZE = 72
FRAME_HEADER_SIZE = 64
AUTH_TOKEN_SIZE = 32
TITLE_ID_SIZE = 9
FRAME_FLAG_SOURCE_OWNED = 1
PIXEL_RGBA8888 = 1
PIXEL_BGRA8888 = 2
PIXEL_RGB565_LE = 3
PIXEL_BYTES = {
    PIXEL_RGBA8888: 4,
    PIXEL_BGRA8888: 4,
    PIXEL_RGB565_LE: 2,
}
_AUTH = struct.Struct(">4sHH32s9s3xIQQ")
_FRAME = struct.Struct(">4sHHQQHHIIIIIQ8s")


class ProtocolError(ValueError):
    """A peer violated the bounded v1 wire contract."""


@dataclasses.dataclass(frozen=True)
class FrameHeader:
    sequence: int
    timestamp_us: int
    width: int
    height: int
    stride_bytes: int
    pixel_format: int
    payload_length: int
    payload_crc32: int
    flags: int
    session_id: int


@dataclasses.dataclass(frozen=True)
class StreamIdentity:
    title_id: str
    process_id: int
    process_generation: int
    session_id: int


def decode_auth(data: bytes, expected_token: bytes) -> StreamIdentity:
    if len(expected_token) != AUTH_TOKEN_SIZE or not any(expected_token):
        raise ValueError("expected token must be exactly 32 nonzero bytes")
    if len(data) != AUTH_SIZE:
        raise ProtocolError("truncated authentication preface")
    (magic, version, size, token, title_id_raw, process_id,
     process_generation, session_id) = _AUTH.unpack(data)
    if magic != AUTH_MAGIC or version != VERSION or size != AUTH_SIZE:
        raise ProtocolError("unsupported authentication preface")
    if data[49:52] != bytes(3):
        raise ProtocolError("nonzero reserved authentication bytes")
    if not any(token):
        raise ProtocolError("zero authentication token")
    import hmac

    if not hmac.compare_digest(token, expected_token):
        raise ProtocolError("authentication failed")
    try:
        title_id = title_id_raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise ProtocolError("title ID is not ASCII") from error
    if (len(title_id) != TITLE_ID_SIZE or
            not all(character.isupper() or character.isdigit()
                    for character in title_id)):
        raise ProtocolError("invalid title ID")
    if process_id == 0 or process_generation == 0 or session_id == 0:
        raise ProtocolError("zero process or session identity")
    return StreamIdentity(
        title_id=title_id,
        process_id=process_id,
        process_generation=process_generation,
        session_id=session_id,
    )


def encode_auth(token: bytes, *, title_id: str = "VDSCRN001",
                process_id: int = 42, process_generation: int = 7,
                session_id: int = 1) -> bytes:
    if len(token) != AUTH_TOKEN_SIZE or not any(token):
        raise ValueError("token must be 32 nonzero bytes")
    try:
        title_id_raw = title_id.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("title ID must be ASCII") from error
    if (len(title_id_raw) != TITLE_ID_SIZE or
            not all(character.isupper() or character.isdigit()
                    for character in title_id)):
        raise ValueError("title ID must be nine uppercase letters/digits")
    if process_id == 0 or process_generation == 0 or session_id == 0:
        raise ValueError("process and session identities must be nonzero")
    return _AUTH.pack(
        AUTH_MAGIC, VERSION, AUTH_SIZE, token, title_id_raw, process_id,
        process_generation, session_id)


def decode_header(data: bytes, *, max_width: int, max_height: int,
                  max_payload: int) -> FrameHeader:
    if len(data) != FRAME_HEADER_SIZE:
        raise ProtocolError("truncated frame header")
    (magic, version, size, sequence, timestamp_us, width, height, stride,
     pixel_format, payload_length, payload_crc32, flags, session_id,
     reserved) = \
        _FRAME.unpack(data)
    if magic != FRAME_MAGIC or version != VERSION or size != FRAME_HEADER_SIZE:
        raise ProtocolError("unsupported frame header")
    if reserved != bytes(8):
        raise ProtocolError("nonzero reserved frame bytes")
    if session_id == 0:
        raise ProtocolError("zero frame session identity")
    if flags != FRAME_FLAG_SOURCE_OWNED:
        raise ProtocolError("unsupported or missing source-owned frame flag")
    bytes_per_pixel = PIXEL_BYTES.get(pixel_format)
    if bytes_per_pixel is None:
        raise ProtocolError("unsupported pixel format")
    if not 1 <= width <= max_width or not 1 <= height <= max_height:
        raise ProtocolError("frame dimensions exceed configured limits")
    if stride < width * bytes_per_pixel:
        raise ProtocolError("frame stride is smaller than one visible row")
    expected_length = stride * height
    if payload_length != expected_length:
        raise ProtocolError("payload length does not match stride and height")
    if not 1 <= payload_length <= max_payload:
        raise ProtocolError("frame payload exceeds configured limit")
    return FrameHeader(
        sequence=sequence,
        timestamp_us=timestamp_us,
        width=width,
        height=height,
        stride_bytes=stride,
        pixel_format=pixel_format,
        payload_length=payload_length,
        payload_crc32=payload_crc32,
        flags=flags,
        session_id=session_id,
    )


def encode_frame(header: FrameHeader, payload: bytes) -> bytes:
    if len(payload) != header.payload_length:
        raise ValueError("mock payload length mismatch")
    packed = _FRAME.pack(
        FRAME_MAGIC,
        VERSION,
        FRAME_HEADER_SIZE,
        header.sequence,
        header.timestamp_us,
        header.width,
        header.height,
        header.stride_bytes,
        header.pixel_format,
        header.payload_length,
        zlib.crc32(payload),
        header.flags,
        header.session_id,
        bytes(8),
    )
    return packed + payload
