"""Canonical, read-only protocol for a future VitaDebugger attach broker.

Version 1 intentionally has no operation that loads a module, suspends a
process, writes memory, or starts GDB.  It can negotiate read-only capabilities,
resolve one exact title ID to an identity-bound ticket, and release that ticket.
"""

from __future__ import annotations

import re
import secrets
import struct
import unicodedata
from dataclasses import dataclass
from urllib.parse import quote_from_bytes

from .errors import ProtocolError


WIRE_VERSION = 1
MAX_FRAME_SIZE = 4096
CURRENT_KERNEL_ABI = 0x0001000E
MIN_TICKET_LEASE_MS = 250
MAX_TICKET_LEASE_MS = 60_000

CAP_STATUS = 1 << 0
CAP_EXACT_TITLE_DISCOVERY = 1 << 1
CAP_IDENTITY_TICKET = 1 << 2
CAP_TICKET_RELEASE = 1 << 3
READ_ONLY_CAPABILITIES = (
    CAP_STATUS
    | CAP_EXACT_TITLE_DISCOVERY
    | CAP_IDENTITY_TICKET
    | CAP_TICKET_RELEASE
)
KNOWN_CAPABILITIES = READ_ONLY_CAPABILITIES

MODE_OBSERVE = "observe"
CONTROL_POLICY_DISABLED = "disabled"

HELLO_HEADER = "VITADEBUG-ATTACH-HELLO-1"
HELLO_RESULT_HEADER = "VITADEBUG-ATTACH-HELLO-RESULT-1"
DISCOVER_HEADER = "VITADEBUG-ATTACH-DISCOVER-1"
DISCOVER_RESULT_HEADER = "VITADEBUG-ATTACH-DISCOVER-RESULT-1"
RELEASE_HEADER = "VITADEBUG-ATTACH-RELEASE-1"
RELEASE_RESULT_HEADER = "VITADEBUG-ATTACH-RELEASE-RESULT-1"

HELLO_KEYS = (
    "request_id",
    "client_nonce",
    "mode",
    "expected_kernel_abi",
    "required_caps",
)
HELLO_RESULT_KEYS = (
    "request_id",
    "client_nonce",
    "server_nonce",
    "service_generation",
    "state",
    "attach_caps",
    "kernel_abi",
    "kernel_caps",
    "ticket_lease_ms",
    "control_policy",
    "message",
)
DISCOVER_KEYS = (
    "request_id",
    "server_nonce",
    "service_generation",
    "target_title_id",
    "mode",
)
DISCOVER_RESULT_KEYS = (
    "request_id",
    "server_nonce",
    "service_generation",
    "state",
    "target_title_id",
    "pid",
    "main_modid",
    "main_fingerprint",
    "target_generation",
    "target_ticket",
    "message",
)
RELEASE_KEYS = (
    "request_id",
    "server_nonce",
    "service_generation",
    "target_ticket",
)
RELEASE_RESULT_KEYS = (
    "request_id",
    "server_nonce",
    "service_generation",
    "target_ticket",
    "state",
    "message",
)

_TITLE_ID = re.compile(r"[A-Z0-9]{9}\Z", re.ASCII)
_LOWER_HEX = re.compile(r"[0-9a-f]+\Z", re.ASCII)
_DISCOVERY_STATES = frozenset({"found", "not_found", "denied", "changed", "error"})
_RELEASE_STATES = frozenset({"released", "missing", "changed", "error"})
_UNSAFE_BIDI_CLASSES = frozenset({
    "BN", "LRE", "RLE", "LRO", "RLO", "PDF", "LRI", "RLI", "FSI",
    "PDI",
})


def validate_title_id(value: str) -> str:
    if not isinstance(value, str) or _TITLE_ID.fullmatch(value) is None:
        raise ProtocolError("title ID must be exactly nine uppercase ASCII letters or digits")
    return value


def _hex_value(value: str, digits: int, label: str, *, allow_zero: bool = True) -> int:
    if len(value) != digits or _LOWER_HEX.fullmatch(value) is None:
        raise ProtocolError(f"{label} must be exactly {digits} lowercase hexadecimal digits")
    number = int(value, 16)
    if not allow_zero and number == 0:
        raise ProtocolError(f"{label} must not be zero")
    return number


def _hex_text(value: int, digits: int, label: str, *, allow_zero: bool = True) -> str:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError(f"{label} must be an integer")
    if value < 0 or value >= 1 << (digits * 4):
        raise ProtocolError(f"{label} is outside its {digits * 4}-bit range")
    if not allow_zero and value == 0:
        raise ProtocolError(f"{label} must not be zero")
    return f"{value:0{digits}x}"


def _token(value: str, digits: int, label: str, *, allow_zero: bool = False) -> str:
    _hex_value(value, digits, label, allow_zero=allow_zero)
    return value


def _canonical_uint(value: str, label: str, *, minimum: int, maximum: int) -> int:
    if value == "0":
        number = 0
    elif not value or value.startswith("0") or not value.isascii() or not value.isdecimal():
        raise ProtocolError(f"{label} is not canonical unsigned decimal")
    else:
        number = int(value, 10)
    if not minimum <= number <= maximum:
        raise ProtocolError(f"{label} must be from {minimum} through {maximum}")
    return number


def _percent_encode(message: str) -> str:
    if not isinstance(message, str):
        raise ProtocolError("message must be text")
    _validate_display_text(message)
    encoded = quote_from_bytes(message.encode("utf-8"), safe="-._~")
    if not encoded:
        return "%00"
    return encoded


def _percent_decode(value: str) -> str:
    if value == "%00":
        return ""
    output = bytearray()
    index = 0
    while index < len(value):
        char = value[index]
        if char == "%":
            if index + 2 >= len(value):
                raise ProtocolError("message has a truncated percent escape")
            try:
                output.append(int(value[index + 1 : index + 3], 16))
            except ValueError as exc:
                raise ProtocolError("message has an invalid percent escape") from exc
            index += 3
            continue
        codepoint = ord(char)
        if codepoint < 0x20 or codepoint > 0x7E:
            raise ProtocolError("message contains non-ASCII wire text")
        output.append(codepoint)
        index += 1
    try:
        message = output.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError("message is not valid percent-encoded UTF-8") from exc
    _validate_display_text(message)
    if _percent_encode(message) != value:
        raise ProtocolError("message is not canonically percent-encoded")
    return message


def _validate_display_text(message: str) -> None:
    for character in message:
        if (unicodedata.category(character) in {"Cc", "Cf", "Cs", "Zl", "Zp"} or
                unicodedata.bidirectional(character) in _UNSAFE_BIDI_CLASSES):
            raise ProtocolError(
                "message contains unsafe terminal or bidirectional controls")


def _record(header: str, keys: tuple[str, ...], values: dict[str, str]) -> bytes:
    if set(values) != set(keys):
        raise ProtocolError(f"{header} fields do not match the protocol schema")
    rows = [header, *(f"{key}={values[key]}" for key in keys)]
    data = ("\n".join(rows) + "\n").encode("ascii", "strict")
    if len(data) > MAX_FRAME_SIZE:
        raise ProtocolError(f"{header} record exceeds {MAX_FRAME_SIZE} bytes")
    return data


def _parse_record(data: bytes, header: str, keys: tuple[str, ...], label: str) -> dict[str, str]:
    if not isinstance(data, bytes):
        raise ProtocolError(f"{label} must be bytes")
    if not data or len(data) > MAX_FRAME_SIZE:
        raise ProtocolError(f"{label} size is invalid")
    if not data.endswith(b"\n") or b"\r" in data or b"\x00" in data:
        raise ProtocolError(f"{label} must be LF-only ASCII ending in LF")
    if any(byte != 0x0A and not 0x20 <= byte <= 0x7E for byte in data):
        raise ProtocolError(f"{label} contains non-printable wire text")
    try:
        decoded = data.decode("ascii", "strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError(f"{label} is not ASCII") from exc
    lines = decoded[:-1].split("\n")
    if not lines or lines[0] != header:
        raise ProtocolError(f"invalid {label} header")
    if len(lines) != len(keys) + 1:
        raise ProtocolError(f"{label} contains missing or extra fields")
    values: dict[str, str] = {}
    for line, key in zip(lines[1:], keys):
        prefix = key + "="
        if not line.startswith(prefix):
            raise ProtocolError(f"{label} field order is invalid; expected {key}")
        value = line[len(prefix) :]
        if not value:
            raise ProtocolError(f"{label} field {key} is empty")
        values[key] = value
    return values


def frame_record(data: bytes) -> bytes:
    if not isinstance(data, bytes) or not 1 <= len(data) <= MAX_FRAME_SIZE:
        raise ProtocolError(f"frame payload must contain 1 through {MAX_FRAME_SIZE} bytes")
    return struct.pack(">I", len(data)) + data


def parse_frame_prefix(prefix: bytes) -> int:
    if not isinstance(prefix, bytes) or len(prefix) != 4:
        raise ProtocolError("frame prefix must contain exactly four bytes")
    length = struct.unpack(">I", prefix)[0]
    if not 1 <= length <= MAX_FRAME_SIZE:
        raise ProtocolError(f"frame length must be from 1 through {MAX_FRAME_SIZE}")
    return length


@dataclass(frozen=True)
class HelloRequest:
    request_id: str
    client_nonce: str
    expected_kernel_abi: int = CURRENT_KERNEL_ABI
    required_caps: int = READ_ONLY_CAPABILITIES

    @property
    def data(self) -> bytes:
        if self.required_caps & ~KNOWN_CAPABILITIES:
            raise ProtocolError("required capabilities contain unsupported version-1 bits")
        return _record(
            HELLO_HEADER,
            HELLO_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "client_nonce": _token(self.client_nonce, 64, "client nonce"),
                "mode": MODE_OBSERVE,
                "expected_kernel_abi": _hex_text(
                    self.expected_kernel_abi, 8, "expected kernel ABI", allow_zero=False
                ),
                "required_caps": _hex_text(self.required_caps, 8, "required capabilities"),
            },
        )


def make_hello_request(
    *,
    expected_kernel_abi: int = CURRENT_KERNEL_ABI,
    required_caps: int = READ_ONLY_CAPABILITIES,
    request_id: str | None = None,
    client_nonce: str | None = None,
) -> HelloRequest:
    if required_caps & ~KNOWN_CAPABILITIES:
        raise ProtocolError("required capabilities contain unsupported version-1 bits")
    request = HelloRequest(
        request_id=request_id or secrets.token_hex(16),
        client_nonce=client_nonce or secrets.token_hex(32),
        expected_kernel_abi=expected_kernel_abi,
        required_caps=required_caps,
    )
    request.data
    return request


def parse_hello_request(data: bytes) -> HelloRequest:
    values = _parse_record(data, HELLO_HEADER, HELLO_KEYS, "hello request")
    if values["mode"] != MODE_OBSERVE:
        raise ProtocolError("version-1 hello mode must be observe")
    required_caps = _hex_value(values["required_caps"], 8, "required capabilities")
    if required_caps & ~KNOWN_CAPABILITIES:
        raise ProtocolError("required capabilities contain unsupported version-1 bits")
    return HelloRequest(
        request_id=_token(values["request_id"], 32, "request ID"),
        client_nonce=_token(values["client_nonce"], 64, "client nonce"),
        expected_kernel_abi=_hex_value(
            values["expected_kernel_abi"], 8, "expected kernel ABI", allow_zero=False
        ),
        required_caps=required_caps,
    )


@dataclass(frozen=True)
class HelloResult:
    request_id: str
    client_nonce: str
    server_nonce: str
    service_generation: int
    state: str
    attach_caps: int
    kernel_abi: int
    kernel_caps: int
    ticket_lease_ms: int
    message: str = ""

    @property
    def data(self) -> bytes:
        if self.state not in {"ready", "unavailable"}:
            raise ProtocolError("hello state must be ready or unavailable")
        if self.attach_caps & ~KNOWN_CAPABILITIES:
            raise ProtocolError("attach capabilities contain unsupported version-1 bits")
        return _record(
            HELLO_RESULT_HEADER,
            HELLO_RESULT_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "client_nonce": _token(self.client_nonce, 64, "client nonce"),
                "server_nonce": _token(self.server_nonce, 64, "server nonce"),
                "service_generation": _hex_text(
                    self.service_generation, 16, "service generation", allow_zero=False
                ),
                "state": self.state,
                "attach_caps": _hex_text(self.attach_caps, 8, "attach capabilities"),
                "kernel_abi": _hex_text(self.kernel_abi, 8, "kernel ABI"),
                "kernel_caps": _hex_text(self.kernel_caps, 8, "kernel capabilities"),
                "ticket_lease_ms": str(
                    _checked_lease(self.ticket_lease_ms)
                ),
                "control_policy": CONTROL_POLICY_DISABLED,
                "message": _percent_encode(self.message),
            },
        )


def _checked_lease(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError("ticket lease must be an integer")
    if not MIN_TICKET_LEASE_MS <= value <= MAX_TICKET_LEASE_MS:
        raise ProtocolError(
            f"ticket lease must be from {MIN_TICKET_LEASE_MS} through {MAX_TICKET_LEASE_MS} ms"
        )
    return value


def parse_hello_result(data: bytes) -> HelloResult:
    values = _parse_record(data, HELLO_RESULT_HEADER, HELLO_RESULT_KEYS, "hello result")
    if values["state"] not in {"ready", "unavailable"}:
        raise ProtocolError("hello state must be ready or unavailable")
    if values["control_policy"] != CONTROL_POLICY_DISABLED:
        raise ProtocolError("version-1 control policy must remain disabled")
    attach_caps = _hex_value(values["attach_caps"], 8, "attach capabilities")
    if attach_caps & ~KNOWN_CAPABILITIES:
        raise ProtocolError("attach capabilities contain unsupported version-1 bits")
    return HelloResult(
        request_id=_token(values["request_id"], 32, "request ID"),
        client_nonce=_token(values["client_nonce"], 64, "client nonce"),
        server_nonce=_token(values["server_nonce"], 64, "server nonce"),
        service_generation=_hex_value(
            values["service_generation"], 16, "service generation", allow_zero=False
        ),
        state=values["state"],
        attach_caps=attach_caps,
        kernel_abi=_hex_value(values["kernel_abi"], 8, "kernel ABI"),
        kernel_caps=_hex_value(values["kernel_caps"], 8, "kernel capabilities"),
        ticket_lease_ms=_canonical_uint(
            values["ticket_lease_ms"],
            "ticket lease",
            minimum=MIN_TICKET_LEASE_MS,
            maximum=MAX_TICKET_LEASE_MS,
        ),
        message=_percent_decode(values["message"]),
    )


@dataclass(frozen=True)
class DiscoverRequest:
    request_id: str
    server_nonce: str
    service_generation: int
    target_title_id: str

    @property
    def data(self) -> bytes:
        return _record(
            DISCOVER_HEADER,
            DISCOVER_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "server_nonce": _token(self.server_nonce, 64, "server nonce"),
                "service_generation": _hex_text(
                    self.service_generation, 16, "service generation", allow_zero=False
                ),
                "target_title_id": validate_title_id(self.target_title_id),
                "mode": MODE_OBSERVE,
            },
        )


def make_discover_request(
    *,
    server_nonce: str,
    service_generation: int,
    target_title_id: str,
    request_id: str | None = None,
) -> DiscoverRequest:
    request = DiscoverRequest(
        request_id=request_id or secrets.token_hex(16),
        server_nonce=server_nonce,
        service_generation=service_generation,
        target_title_id=target_title_id,
    )
    request.data
    return request


def parse_discover_request(data: bytes) -> DiscoverRequest:
    values = _parse_record(data, DISCOVER_HEADER, DISCOVER_KEYS, "discover request")
    if values["mode"] != MODE_OBSERVE:
        raise ProtocolError("version-1 discovery mode must be observe")
    return DiscoverRequest(
        request_id=_token(values["request_id"], 32, "request ID"),
        server_nonce=_token(values["server_nonce"], 64, "server nonce"),
        service_generation=_hex_value(
            values["service_generation"], 16, "service generation", allow_zero=False
        ),
        target_title_id=validate_title_id(values["target_title_id"]),
    )


@dataclass(frozen=True)
class DiscoverResult:
    request_id: str
    server_nonce: str
    service_generation: int
    state: str
    target_title_id: str
    pid: int = 0
    main_modid: int = 0
    main_fingerprint: int = 0
    target_generation: int = 0
    target_ticket: str = "0" * 64
    message: str = ""

    def _validate(self) -> None:
        if self.state not in _DISCOVERY_STATES:
            raise ProtocolError("discovery state is invalid")
        validate_title_id(self.target_title_id)
        _hex_text(self.pid, 8, "PID")
        _hex_text(self.main_modid, 8, "main module ID")
        _hex_text(self.main_fingerprint, 8, "main fingerprint")
        _hex_text(self.target_generation, 16, "target generation")
        _token(self.target_ticket, 64, "target ticket", allow_zero=True)
        if self.state == "found":
            if not 1 <= self.pid <= 0x7FFFFFFF:
                raise ProtocolError("found target PID must be a positive SceUID")
            if not 1 <= self.main_modid <= 0x7FFFFFFF:
                raise ProtocolError("found target main module ID must be a positive SceUID")
            if self.main_fingerprint == 0:
                raise ProtocolError("found target requires a nonzero main module fingerprint")
            if self.target_generation == 0 or int(self.target_ticket, 16) == 0:
                raise ProtocolError("found target requires a generation and ticket")
        elif any(
            (
                self.pid,
                self.main_modid,
                self.main_fingerprint,
                self.target_generation,
                int(self.target_ticket, 16),
            )
        ):
            raise ProtocolError("failed discovery must not carry target identity data")

    @property
    def data(self) -> bytes:
        self._validate()
        return _record(
            DISCOVER_RESULT_HEADER,
            DISCOVER_RESULT_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "server_nonce": _token(self.server_nonce, 64, "server nonce"),
                "service_generation": _hex_text(
                    self.service_generation, 16, "service generation", allow_zero=False
                ),
                "state": self.state,
                "target_title_id": self.target_title_id,
                "pid": _hex_text(self.pid, 8, "PID"),
                "main_modid": _hex_text(self.main_modid, 8, "main module ID"),
                "main_fingerprint": _hex_text(
                    self.main_fingerprint, 8, "main fingerprint"
                ),
                "target_generation": _hex_text(
                    self.target_generation, 16, "target generation"
                ),
                "target_ticket": self.target_ticket,
                "message": _percent_encode(self.message),
            },
        )


def parse_discover_result(data: bytes) -> DiscoverResult:
    values = _parse_record(
        data, DISCOVER_RESULT_HEADER, DISCOVER_RESULT_KEYS, "discover result"
    )
    result = DiscoverResult(
        request_id=_token(values["request_id"], 32, "request ID"),
        server_nonce=_token(values["server_nonce"], 64, "server nonce"),
        service_generation=_hex_value(
            values["service_generation"], 16, "service generation", allow_zero=False
        ),
        state=values["state"],
        target_title_id=validate_title_id(values["target_title_id"]),
        pid=_hex_value(values["pid"], 8, "PID"),
        main_modid=_hex_value(values["main_modid"], 8, "main module ID"),
        main_fingerprint=_hex_value(
            values["main_fingerprint"], 8, "main fingerprint"
        ),
        target_generation=_hex_value(
            values["target_generation"], 16, "target generation"
        ),
        target_ticket=_token(
            values["target_ticket"], 64, "target ticket", allow_zero=True
        ),
        message=_percent_decode(values["message"]),
    )
    result._validate()
    return result


@dataclass(frozen=True)
class ReleaseRequest:
    request_id: str
    server_nonce: str
    service_generation: int
    target_ticket: str

    @property
    def data(self) -> bytes:
        return _record(
            RELEASE_HEADER,
            RELEASE_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "server_nonce": _token(self.server_nonce, 64, "server nonce"),
                "service_generation": _hex_text(
                    self.service_generation, 16, "service generation", allow_zero=False
                ),
                "target_ticket": _token(self.target_ticket, 64, "target ticket"),
            },
        )


def make_release_request(
    *,
    server_nonce: str,
    service_generation: int,
    target_ticket: str,
    request_id: str | None = None,
) -> ReleaseRequest:
    request = ReleaseRequest(
        request_id=request_id or secrets.token_hex(16),
        server_nonce=server_nonce,
        service_generation=service_generation,
        target_ticket=target_ticket,
    )
    request.data
    return request


def parse_release_request(data: bytes) -> ReleaseRequest:
    values = _parse_record(data, RELEASE_HEADER, RELEASE_KEYS, "release request")
    return ReleaseRequest(
        request_id=_token(values["request_id"], 32, "request ID"),
        server_nonce=_token(values["server_nonce"], 64, "server nonce"),
        service_generation=_hex_value(
            values["service_generation"], 16, "service generation", allow_zero=False
        ),
        target_ticket=_token(values["target_ticket"], 64, "target ticket"),
    )


@dataclass(frozen=True)
class ReleaseResult:
    request_id: str
    server_nonce: str
    service_generation: int
    target_ticket: str
    state: str
    message: str = ""

    @property
    def data(self) -> bytes:
        if self.state not in _RELEASE_STATES:
            raise ProtocolError("release state is invalid")
        return _record(
            RELEASE_RESULT_HEADER,
            RELEASE_RESULT_KEYS,
            {
                "request_id": _token(self.request_id, 32, "request ID"),
                "server_nonce": _token(self.server_nonce, 64, "server nonce"),
                "service_generation": _hex_text(
                    self.service_generation, 16, "service generation", allow_zero=False
                ),
                "target_ticket": _token(self.target_ticket, 64, "target ticket"),
                "state": self.state,
                "message": _percent_encode(self.message),
            },
        )


def parse_release_result(data: bytes) -> ReleaseResult:
    values = _parse_record(
        data, RELEASE_RESULT_HEADER, RELEASE_RESULT_KEYS, "release result"
    )
    if values["state"] not in _RELEASE_STATES:
        raise ProtocolError("release state is invalid")
    return ReleaseResult(
        request_id=_token(values["request_id"], 32, "request ID"),
        server_nonce=_token(values["server_nonce"], 64, "server nonce"),
        service_generation=_hex_value(
            values["service_generation"], 16, "service generation", allow_zero=False
        ),
        target_ticket=_token(values["target_ticket"], 64, "target ticket"),
        state=values["state"],
        message=_percent_decode(values["message"]),
    )
