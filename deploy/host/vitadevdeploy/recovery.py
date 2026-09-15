"""Read-only recovery classification for interrupted VitaDevDeploy jobs."""

from __future__ import annotations

from dataclasses import asdict, dataclass

from .errors import ProtocolError
from .ftp import DEFAULT_REMOTE_ROOT, VitaFtpClient
from .paths import validate_job_id, validate_sha256, validate_title_id
from .protocol import Result


PROMOTION_STATE_PATH = f"{DEFAULT_REMOTE_ROOT}/promote.state"
PROMOTION_HEADER = "VITADEVDEPLOY-PROMOTE-1"
PROMOTION_FIXED_PATH = "ux0:/data/vdd_pkg"


@dataclass(frozen=True)
class PromotionState:
    job: str
    title_id: str
    manifest_sha256: str
    path: str
    state: str


@dataclass(frozen=True)
class RecoverySnapshot:
    disposition: str
    safe_to_retry: bool
    operator_action: str
    promotion: PromotionState | None
    result: Result | None
    journal_present: bool
    partial_journal_present: bool
    shallow_stage_checked: bool = False

    def to_json(self) -> dict[str, object]:
        value = asdict(self)
        # ``asdict`` already converts the nested frozen dataclasses.
        return value


def _decode_lines(data: bytes) -> list[str]:
    if not data or len(data) > 4096 or not data.endswith(b"\n"):
        raise ProtocolError("promotion state is empty, oversized, or missing final LF")
    try:
        return data.decode("ascii", "strict").splitlines()
    except UnicodeDecodeError as exc:
        raise ProtocolError("promotion state is not ASCII") from exc


def parse_promotion_state(data: bytes) -> PromotionState:
    lines = _decode_lines(data)
    if len(lines) != 6 or lines[0] != PROMOTION_HEADER:
        raise ProtocolError("promotion state layout is invalid")
    expected = ("job", "title_id", "manifest_sha256", "path", "state")
    values: dict[str, str] = {}
    for line, key in zip(lines[1:], expected, strict=True):
        prefix = key + "="
        if not line.startswith(prefix):
            raise ProtocolError("promotion state fields are missing or out of order")
        values[key] = line[len(prefix) :]
    validate_job_id(values["job"])
    validate_title_id(values["title_id"])
    validate_sha256(values["manifest_sha256"])
    if values["path"] != PROMOTION_FIXED_PATH:
        raise ProtocolError("promotion state names an unexpected installer path")
    if values["state"] != "reserved_or_later":
        raise ProtocolError("promotion state has an unknown phase")
    return PromotionState(**values)


def collect_recovery_snapshot(ftp: VitaFtpClient) -> RecoverySnapshot:
    """Classify durable evidence without modifying or launching anything.

    Vita Companion FTP is confined to the deployment root here, so the fixed
    shallow package stage is deliberately reported as unchecked. Any marker is
    therefore a hard retry block even when a success result exists.
    """

    marker_data = ftp.read_optional_bytes(PROMOTION_STATE_PATH, max_size=4096)
    if marker_data is None:
        return RecoverySnapshot(
            disposition="clean",
            safe_to_retry=True,
            operator_action="no promotion recovery marker is present",
            promotion=None,
            result=None,
            journal_present=False,
            partial_journal_present=False,
        )

    promotion = parse_promotion_state(marker_data)
    result = ftp.read_result(promotion.job)
    journal = ftp.read_optional_bytes(
        f"{DEFAULT_REMOTE_ROOT}/results/{promotion.job}.journal", max_size=1024 * 1024
    )
    partial_journal = ftp.read_optional_bytes(
        f"{DEFAULT_REMOTE_ROOT}/results/{promotion.job}.journal.part",
        max_size=1024 * 1024,
    )
    if result is None:
        disposition = "outcome_unknown"
        action = (
            "do not retry; inspect the installed title and ux0:/data/vdd_pkg "
            "before any explicit recovery"
        )
    elif result.succeeded:
        disposition = "success_reported_marker_stale"
        action = (
            "installation success is durable, but verify the shallow stage is absent "
            "before explicitly clearing the stale marker"
        )
    else:
        disposition = "failure_reported_marker_stale"
        action = (
            "do not retry automatically; reconcile the failure stage, journal, installed "
            "title, and shallow package stage"
        )
    return RecoverySnapshot(
        disposition=disposition,
        safe_to_retry=False,
        operator_action=action,
        promotion=promotion,
        result=result,
        journal_present=journal is not None,
        partial_journal_present=partial_journal is not None,
    )


__all__ = [
    "PROMOTION_FIXED_PATH",
    "PROMOTION_HEADER",
    "PROMOTION_STATE_PATH",
    "PromotionState",
    "RecoverySnapshot",
    "collect_recovery_snapshot",
    "parse_promotion_state",
]
