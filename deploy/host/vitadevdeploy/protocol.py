"""Canonical challenge, request, and result wire formats."""

from __future__ import annotations

from dataclasses import dataclass
from urllib.parse import quote_from_bytes

from .errors import ProtocolError
from .paths import validate_job_id, validate_nonce, validate_sha256, validate_title_id

CHALLENGE_HEADER = "VITADEVDEPLOY-CHALLENGE-1"
REQUEST_HEADER = "VITADEVDEPLOY-REQUEST-1"
RESULT_HEADER = "VITADEVDEPLOY-RESULT-1"
SIGNING_DOMAIN = b"VITADEVDEPLOY-SIGNED-JOB-1\x00"
REQUEST_KEYS = (
    "job",
    "nonce",
    "action",
    "title_id",
    "manifest_sha256",
    "file_count",
    "total_size",
)
RESULT_KEYS = ("job", "state", "stage", "code", "title_id", "message")
VALID_ACTIONS = frozenset({"verify", "install", "install_launch"})
DEPLOYER_TITLE_ID = "VDEVDEP01"


def _decode_lines(data: bytes, header: str, *, max_size: int) -> list[str]:
    if len(data) > max_size:
        raise ProtocolError(f"{header} record is too large")
    if not data.endswith(b"\n") or b"\r" in data or b"\x00" in data:
        raise ProtocolError(f"{header} record must use LF-only text and end with LF")
    try:
        lines = data.decode("ascii", "strict").splitlines()
    except UnicodeDecodeError as exc:
        raise ProtocolError(f"{header} record is not ASCII") from exc
    if not lines or lines[0] != header:
        raise ProtocolError(f"invalid {header} header")
    return lines


def _canonical_uint(value: str, label: str) -> int:
    if value == "0":
        return 0
    if not value or value.startswith("0") or not value.isdecimal() or not value.isascii():
        raise ProtocolError(f"{label} is not canonical unsigned decimal")
    return int(value, 10)


def _canonical_int(value: str, label: str) -> int:
    if value == "0":
        return 0
    negative = value.startswith("-")
    digits = value[1:] if negative else value
    if not digits or digits.startswith("0") or not digits.isdecimal() or not digits.isascii():
        raise ProtocolError(f"{label} is not canonical signed decimal")
    return -int(digits, 10) if negative else int(digits, 10)


def _fixed_fields(lines: list[str], keys: tuple[str, ...], label: str) -> dict[str, str]:
    if len(lines) != len(keys) + 1:
        raise ProtocolError(f"{label} contains missing or extra fields")
    values: dict[str, str] = {}
    for line, expected_key in zip(lines[1:], keys):
        prefix = expected_key + "="
        if not line.startswith(prefix):
            raise ProtocolError(f"{label} field order is invalid; expected {expected_key}")
        value = line[len(prefix) :]
        if not value:
            raise ProtocolError(f"{label} field {expected_key} is empty")
        values[expected_key] = value
    return values


@dataclass(frozen=True)
class Challenge:
    nonce: str

    @property
    def data(self) -> bytes:
        return f"{CHALLENGE_HEADER}\nnonce={self.nonce}\n".encode("ascii")


def parse_challenge(data: bytes) -> Challenge:
    lines = _decode_lines(data, CHALLENGE_HEADER, max_size=256)
    values = _fixed_fields(lines, ("nonce",), "challenge")
    return Challenge(nonce=validate_nonce(values["nonce"]))


@dataclass(frozen=True)
class Request:
    job: str
    nonce: str
    action: str
    title_id: str
    manifest_sha256: str
    file_count: int
    total_size: int

    @property
    def data(self) -> bytes:
        rows = [REQUEST_HEADER]
        rows.extend(
            (
                f"job={self.job}",
                f"nonce={self.nonce}",
                f"action={self.action}",
                f"title_id={self.title_id}",
                f"manifest_sha256={self.manifest_sha256}",
                f"file_count={self.file_count}",
                f"total_size={self.total_size}",
            )
        )
        return ("\n".join(rows) + "\n").encode("ascii")


def make_request(
    *,
    job: str,
    nonce: str,
    action: str,
    title_id: str,
    manifest_sha256: str,
    file_count: int,
    total_size: int,
) -> Request:
    validate_job_id(job)
    validate_nonce(nonce)
    validate_title_id(title_id)
    validate_sha256(manifest_sha256)
    if action not in VALID_ACTIONS:
        raise ProtocolError(f"unsupported deployment action: {action!r}")
    if file_count < 0 or total_size < 0:
        raise ProtocolError("request counts cannot be negative")
    return Request(job, nonce, action, title_id, manifest_sha256, file_count, total_size)


def parse_request(data: bytes) -> Request:
    lines = _decode_lines(data, REQUEST_HEADER, max_size=1024)
    values = _fixed_fields(lines, REQUEST_KEYS, "request")
    return make_request(
        job=values["job"],
        nonce=values["nonce"],
        action=values["action"],
        title_id=values["title_id"],
        manifest_sha256=values["manifest_sha256"],
        file_count=_canonical_uint(values["file_count"], "file_count"),
        total_size=_canonical_uint(values["total_size"], "total_size"),
    )


def signed_job_bytes(request: bytes, manifest: bytes) -> bytes:
    return SIGNING_DOMAIN + request + manifest


def percent_encode_message(message: str) -> str:
    return quote_from_bytes(message.encode("utf-8"), safe="-._~")


def _percent_decode_message(value: str) -> str:
    output = bytearray()
    index = 0
    while index < len(value):
        char = value[index]
        if char == "%":
            if index + 2 >= len(value):
                raise ProtocolError("result message has a truncated percent escape")
            try:
                output.append(int(value[index + 1 : index + 3], 16))
            except ValueError as exc:
                raise ProtocolError("result message has an invalid percent escape") from exc
            index += 3
        else:
            if ord(char) < 0x20 or ord(char) > 0x7E:
                raise ProtocolError("result message contains non-ASCII wire text")
            output.append(ord(char))
            index += 1
    try:
        return output.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        raise ProtocolError("result message is not valid percent-encoded UTF-8") from exc


@dataclass(frozen=True)
class Result:
    job: str
    state: str
    stage: str
    code: int
    title_id: str
    message: str

    @property
    def terminal(self) -> bool:
        return self.state in {"success", "failed"}

    @property
    def succeeded(self) -> bool:
        return self.state == "success" and self.code == 0


def parse_result(data: bytes) -> Result:
    lines = _decode_lines(data, RESULT_HEADER, max_size=4096)
    values = _fixed_fields(lines, RESULT_KEYS, "result")
    validate_job_id(values["job"])
    if values["state"] not in {"success", "failed"}:
        raise ProtocolError("result state is invalid")
    if not values["stage"].isascii() or not values["stage"].replace("_", "").isalnum() or values["stage"].lower() != values["stage"]:
        raise ProtocolError("result stage must be a lowercase ASCII token")
    code = _canonical_int(values["code"], "result code")
    if not -(2**31) <= code <= 2**31 - 1:
        raise ProtocolError("result code is outside int32 range")
    validate_title_id(values["title_id"])
    result = Result(
        job=values["job"],
        state=values["state"],
        stage=values["stage"],
        code=code,
        title_id=values["title_id"],
        message=_percent_decode_message(values["message"]),
    )
    if result.state == "success" and result.code != 0:
        raise ProtocolError("successful result must have code zero")
    if result.state == "failed" and result.code == 0:
        raise ProtocolError("failed result must have a nonzero code")
    return result
