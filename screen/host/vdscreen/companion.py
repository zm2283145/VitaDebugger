"""Deterministic VitaDebugger companion control protocol v1 codec."""

from __future__ import annotations

import dataclasses
import hashlib
import hmac
import ipaddress
import struct


MAGIC = b"VDCP"
VERSION = 1
HEADER_SIZE = 80
TAG_SIZE = 16
SECRET_SIZE = 32
MAX_PAYLOAD = 4096
MAX_TTL_MS = 5000
RESPONSE_BIT = 0x8000
MESSAGE_HELLO = 1
MESSAGE_STATUS = 2
MESSAGE_INPUT = 3
MESSAGE_FILE_LIST = 4
MESSAGE_FILE_READ = 5
MESSAGE_TITLE_INVENTORY = 6
MESSAGE_TITLE_LAUNCH = 7
CAP_STATUS = 1
CAP_APP_INPUT = 2
CAP_DEBUG_FILES = 4
CAP_SCREEN = 8
CAP_INPUT_RECORD = 16
CAP_INPUT_PLAYBACK = 32
CAP_SUPPORTED = (
    CAP_STATUS
    | CAP_APP_INPUT
    | CAP_DEBUG_FILES
    | CAP_SCREEN
    | CAP_INPUT_RECORD
    | CAP_INPUT_PLAYBACK
)
MAC_DOMAIN = b"VITADEBUG-COMPANION/RECORD/v1"
ENDPOINT_CONFIG_MAGIC = b"VDCG"
ENDPOINT_CONFIG_VERSION = 1
ENDPOINT_CONFIG_SIZE = 128
ENDPOINT_CONTROL_PORT = 18198
ENDPOINT_SCREEN_PORT = 18197
NETWORK_LOOPBACK = 1
NETWORK_PRIVATE_LAN = 2
EXPLICIT_CONSENT = 0x56444350
LAN_CONSENT = 0x56444C41
MUTATION_CONSENT = 0x56444D55
RECORD_CONSENT = 0x56445243
PLAYBACK_CONSENT = 0x56445042
_HEADER = struct.Struct(">4sHHHHIQQQQQQ16s")
_STATUS = struct.Struct(">9s3xIQQQIIQQII")
_RFC1918_NETWORKS = (
    ipaddress.IPv4Network("10.0.0.0/8"),
    ipaddress.IPv4Network("172.16.0.0/12"),
    ipaddress.IPv4Network("192.168.0.0/16"),
)


class CompanionProtocolError(ValueError):
    """A record violates the bounded authenticated VDCP-v1 contract."""


@dataclasses.dataclass(frozen=True)
class CompanionIdentity:
    title_id: str
    process_id: int
    process_generation: int
    session_id: int


@dataclasses.dataclass(frozen=True)
class CompanionRecord:
    message_type: int
    status: int
    sequence: int
    ttl_ms: int
    session_id: int
    process_generation: int
    capabilities: int
    payload: bytes


@dataclasses.dataclass(frozen=True)
class CompanionStatus:
    identity: CompanionIdentity
    capabilities: int
    trace_state: int
    trace_event_count: int
    trace_duration_us: int
    trace_max_scheduling_drift_us: int
    input_cleanup_pending: bool
    last_error: int


@dataclasses.dataclass(frozen=True)
class EndpointConfig:
    host: ipaddress.IPv4Address
    bind_address: ipaddress.IPv4Address
    network_scope: int
    capabilities: int
    control_secret: bytes
    screen_secret: bytes


def _validate_secret(secret: bytes) -> bytes:
    secret = bytes(secret)
    if len(secret) != SECRET_SIZE or not any(secret):
        raise ValueError("control secret must be exactly 32 nonzero bytes")
    return secret


def _is_rfc1918(address: ipaddress.IPv4Address) -> bool:
    return any(address in network for network in _RFC1918_NETWORKS)


def _validate_record_fields(
    *,
    message_type: int,
    status: int,
    sequence: int,
    ttl_ms: int,
    session_id: int,
    process_generation: int,
    capabilities: int,
    payload: bytes,
) -> None:
    if not 0 < message_type <= 0xFFFF:
        raise ValueError("message type must be a nonzero u16")
    if not 0 <= status <= 0xFFFF:
        raise ValueError("status must be a u16")
    if not 0 < sequence <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("sequence must be a nonzero u64")
    if not 0 < ttl_ms <= MAX_TTL_MS:
        raise ValueError("TTL must be between 1 and 5000 milliseconds")
    if not 0 < session_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("session ID must be a nonzero u64")
    if not 0 < process_generation <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("process generation must be a nonzero u64")
    if not 0 <= capabilities <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("capabilities must be a u64")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload exceeds the 4096-byte limit")


def encode_record(
    secret: bytes,
    *,
    message_type: int,
    sequence: int,
    ttl_ms: int,
    session_id: int,
    process_generation: int,
    capabilities: int,
    payload: bytes = b"",
    status: int = 0,
) -> bytes:
    """Encode one canonical authenticated record without network access."""
    secret = _validate_secret(secret)
    payload = bytes(payload)
    _validate_record_fields(
        message_type=message_type,
        status=status,
        sequence=sequence,
        ttl_ms=ttl_ms,
        session_id=session_id,
        process_generation=process_generation,
        capabilities=capabilities,
        payload=payload,
    )
    prefix = struct.pack(
        ">4sHHHHIQQQQQQ",
        MAGIC,
        VERSION,
        HEADER_SIZE,
        message_type,
        status,
        len(payload),
        sequence,
        ttl_ms,
        session_id,
        process_generation,
        capabilities,
        0,
    )
    tag = hashlib.blake2b(
        MAC_DOMAIN + prefix + payload,
        digest_size=TAG_SIZE,
        key=secret,
    ).digest()
    return prefix + tag + payload


def decode_record(secret: bytes, data: bytes) -> CompanionRecord:
    """Authenticate and decode one complete bounded record."""
    secret = _validate_secret(secret)
    data = bytes(data)
    if not HEADER_SIZE <= len(data) <= HEADER_SIZE + MAX_PAYLOAD:
        raise CompanionProtocolError("record size is outside fixed bounds")
    (
        magic,
        version,
        header_size,
        message_type,
        status,
        payload_size,
        sequence,
        ttl_ms,
        session_id,
        generation,
        capabilities,
        reserved,
        supplied_tag,
    ) = _HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise CompanionProtocolError("record version or header size is invalid")
    if reserved != 0:
        raise CompanionProtocolError("record reserved field must be zero")
    if payload_size > MAX_PAYLOAD or len(data) != HEADER_SIZE + payload_size:
        raise CompanionProtocolError("record payload size is inconsistent")
    payload = data[HEADER_SIZE:]
    try:
        _validate_record_fields(
            message_type=message_type,
            status=status,
            sequence=sequence,
            ttl_ms=ttl_ms,
            session_id=session_id,
            process_generation=generation,
            capabilities=capabilities,
            payload=payload,
        )
    except ValueError as error:
        raise CompanionProtocolError(str(error)) from error
    expected_tag = hashlib.blake2b(
        MAC_DOMAIN + data[:64] + payload,
        digest_size=TAG_SIZE,
        key=secret,
    ).digest()
    if not hmac.compare_digest(supplied_tag, expected_tag):
        raise CompanionProtocolError("record authentication failed")
    return CompanionRecord(
        message_type,
        status,
        sequence,
        ttl_ms,
        session_id,
        generation,
        capabilities,
        payload,
    )


def validate_response(
    record: CompanionRecord,
    *,
    request_type: int,
    sequence: int,
    identity: CompanionIdentity,
    capabilities: int,
) -> None:
    """Validate response correlation and exact live target binding."""
    if record.message_type != request_type | RESPONSE_BIT:
        raise CompanionProtocolError("response type does not match request")
    if record.sequence != sequence:
        raise CompanionProtocolError("response sequence does not match request")
    if record.session_id != identity.session_id:
        raise CompanionProtocolError("response session ID does not match")
    if record.process_generation != identity.process_generation:
        raise CompanionProtocolError("response process generation does not match")
    if record.capabilities != capabilities:
        raise CompanionProtocolError("response capabilities do not match")


def decode_status(payload: bytes) -> CompanionStatus:
    """Decode the exact 72-byte status payload."""
    if len(payload) != _STATUS.size:
        raise CompanionProtocolError("status payload must be exactly 72 bytes")
    (
        title_raw,
        process_id,
        generation,
        session_id,
        capabilities,
        trace_state,
        trace_event_count,
        trace_duration_us,
        trace_max_drift_us,
        cleanup_pending,
        last_error,
    ) = _STATUS.unpack(payload)
    try:
        title_id = title_raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise CompanionProtocolError("status title ID is not ASCII") from error
    if (
        len(title_id) != 9
        or not all(character.isupper() or character.isdigit()
                   for character in title_id)
        or not process_id
        or not generation
        or not session_id
    ):
        raise CompanionProtocolError("status identity is invalid")
    if capabilities & ~CAP_SUPPORTED:
        raise CompanionProtocolError("status contains unsupported capabilities")
    if cleanup_pending not in (0, 1):
        raise CompanionProtocolError("status cleanup flag is invalid")
    return CompanionStatus(
        CompanionIdentity(title_id, process_id, generation, session_id),
        capabilities,
        trace_state,
        trace_event_count,
        trace_duration_us,
        trace_max_drift_us,
        bool(cleanup_pending),
        last_error,
    )


def encode_endpoint_config(
    *,
    host: str | ipaddress.IPv4Address,
    capabilities: int,
    control_secret: bytes,
    screen_secret: bytes,
    allow_lan: bool = False,
    bind_address: str | ipaddress.IPv4Address | None = None,
) -> bytes:
    """Build the endpoint's fixed-size, explicit-consent offline config."""
    address = ipaddress.IPv4Address(host)
    control_secret = _validate_secret(control_secret)
    screen_secret = _validate_secret(screen_secret)
    if hmac.compare_digest(control_secret, screen_secret):
        raise ValueError("control and screen secrets must be distinct")
    if (
        capabilities & CAP_STATUS == 0
        or capabilities & ~CAP_SUPPORTED
    ):
        raise ValueError("capabilities must be a supported mask including status")
    if address.is_loopback:
        if address != ipaddress.IPv4Address("127.0.0.1"):
            raise ValueError("loopback endpoint must be exactly 127.0.0.1")
        network_scope = NETWORK_LOOPBACK
        local_address = ipaddress.IPv4Address("127.0.0.1")
    else:
        if not allow_lan:
            raise ValueError("private-LAN endpoint requires explicit allow_lan")
        if not _is_rfc1918(address):
            raise ValueError("endpoint host must be loopback or RFC1918 IPv4")
        if bind_address is None:
            raise ValueError(
                "private-LAN endpoint requires an explicit Vita bind address")
        local_address = ipaddress.IPv4Address(bind_address)
        if not _is_rfc1918(local_address):
            raise ValueError("Vita bind address must be RFC1918 IPv4")
        if local_address == address:
            raise ValueError(
                "Vita bind address must differ from the remote host")
        network_scope = NETWORK_PRIVATE_LAN

    mutation = bool(capabilities & (CAP_APP_INPUT | CAP_INPUT_PLAYBACK))
    recording = bool(capabilities & CAP_INPUT_RECORD)
    playback = bool(capabilities & CAP_INPUT_PLAYBACK)
    output = bytearray(ENDPOINT_CONFIG_SIZE)
    struct.pack_into(
        ">4sHHIIIIIIQQ4sHH32s32s4s4x",
        output,
        0,
        ENDPOINT_CONFIG_MAGIC,
        ENDPOINT_CONFIG_VERSION,
        ENDPOINT_CONFIG_SIZE,
        EXPLICIT_CONSENT,
        LAN_CONSENT if network_scope == NETWORK_PRIVATE_LAN else 0,
        MUTATION_CONSENT if mutation else 0,
        RECORD_CONSENT if recording else 0,
        PLAYBACK_CONSENT if playback else 0,
        network_scope,
        capabilities,
        capabilities,
        address.packed,
        ENDPOINT_CONTROL_PORT,
        ENDPOINT_SCREEN_PORT,
        control_secret,
        screen_secret,
        local_address.packed,
    )
    return bytes(output)


def decode_endpoint_config(data: bytes) -> EndpointConfig:
    """Verify and decode a complete endpoint config exactly as the Vita does."""
    data = bytes(data)
    if len(data) != ENDPOINT_CONFIG_SIZE:
        raise CompanionProtocolError(
            "endpoint config must be exactly 128 bytes")
    (
        magic,
        version,
        size,
        explicit,
        lan,
        mutation,
        recording,
        playback,
        network_scope,
        capabilities,
        consented,
        address_raw,
        control_port,
        screen_port,
        control_secret,
        screen_secret,
        bind_raw,
    ) = struct.unpack(">4sHHIIIIIIQQ4sHH32s32s4s4x", data)
    if (
        magic != ENDPOINT_CONFIG_MAGIC
        or version != ENDPOINT_CONFIG_VERSION
        or size != ENDPOINT_CONFIG_SIZE
        or explicit != EXPLICIT_CONSENT
        or any(data[124:])
    ):
        raise CompanionProtocolError("endpoint config framing is invalid")
    try:
        canonical = encode_endpoint_config(
            host=ipaddress.IPv4Address(address_raw),
            capabilities=capabilities,
            control_secret=control_secret,
            screen_secret=screen_secret,
            allow_lan=network_scope == NETWORK_PRIVATE_LAN,
            bind_address=ipaddress.IPv4Address(bind_raw),
        )
    except ValueError as error:
        raise CompanionProtocolError(str(error)) from error
    if not hmac.compare_digest(canonical, data):
        raise CompanionProtocolError(
            "endpoint config consent, ports, or scope are invalid")
    if consented != capabilities or control_port != ENDPOINT_CONTROL_PORT or \
            screen_port != ENDPOINT_SCREEN_PORT:
        raise CompanionProtocolError("endpoint config policy is invalid")
    expected_mutation = (
        MUTATION_CONSENT
        if capabilities & (CAP_APP_INPUT | CAP_INPUT_PLAYBACK)
        else 0
    )
    expected_recording = (
        RECORD_CONSENT if capabilities & CAP_INPUT_RECORD else 0)
    expected_playback = (
        PLAYBACK_CONSENT if capabilities & CAP_INPUT_PLAYBACK else 0)
    expected_lan = LAN_CONSENT if network_scope == NETWORK_PRIVATE_LAN else 0
    if (mutation, recording, playback, lan) != (
        expected_mutation,
        expected_recording,
        expected_playback,
        expected_lan,
    ):
        raise CompanionProtocolError("endpoint config consents are invalid")
    return EndpointConfig(
        ipaddress.IPv4Address(address_raw),
        ipaddress.IPv4Address(bind_raw),
        network_scope,
        capabilities,
        control_secret,
        screen_secret,
    )
