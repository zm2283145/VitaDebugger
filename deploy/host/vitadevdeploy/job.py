"""Build and verify local deployment job directories."""

from __future__ import annotations

import os
import secrets
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .crypto import sign, verify
from .errors import ProtocolError, VpkValidationError
from .headbin import create_head_bin
from .manifest import Manifest, build_manifest, parse_manifest, verify_manifest
from .paths import validate_job_id, validate_nonce, validate_title_id
from .protocol import DEPLOYER_TITLE_ID, Request, make_request, parse_request, signed_job_bytes
from .sfo import parse_sfo
from .vpk import VpkInspection, VpkLimits, extract_vpk


@dataclass(frozen=True)
class PreparedJob:
    root: Path
    package: Path
    manifest_path: Path
    request_path: Path
    signature_path: Path
    request: Request
    manifest: Manifest
    inspection: VpkInspection


def resolve_head_template(path: os.PathLike[str] | str | None) -> Path:
    if path is not None:
        selected = Path(path)
    else:
        selected = Path(__file__).resolve().parents[2] / "third_party" / "vitashell" / "head.bin"
    if not selected.is_file():
        raise VpkValidationError(
            f"VitaShell head.bin template not found at {selected}; pass --head-template"
        )
    return selected


def prepare_job(
    *,
    vpk_path: os.PathLike[str] | str,
    output_directory: os.PathLike[str] | str,
    nonce: str,
    private_key: os.PathLike[str] | str,
    head_template: os.PathLike[str] | str | None = None,
    action: str = "install_launch",
    installer_title_id: str = DEPLOYER_TITLE_ID,
    job_id: str | None = None,
    crypto_backend: str = "auto",
    openssl_executable: str | None = None,
    limits: VpkLimits | None = None,
) -> PreparedJob:
    validate_nonce(nonce)
    validate_title_id(installer_title_id)
    selected_job = validate_job_id(job_id or secrets.token_hex(16))
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    final_root = output / selected_job
    if final_root.exists():
        raise ProtocolError(f"job output already exists: {final_root}")
    temp_root = Path(tempfile.mkdtemp(prefix=f".{selected_job}-", dir=output))
    package = temp_root / "package"
    try:
        inspection = extract_vpk(vpk_path, package, limits)
        if inspection.title_id == installer_title_id:
            raise ProtocolError("the running installer cannot update itself")
        create_head_bin(package, resolve_head_template(head_template), overwrite=True)
        manifest = build_manifest(
            package,
            max_files=(limits or VpkLimits()).max_entries,
            max_total_size=(limits or VpkLimits()).max_total_size,
        )
        request = make_request(
            job=selected_job,
            nonce=nonce,
            action=action,
            title_id=inspection.title_id,
            manifest_sha256=manifest.sha256,
            file_count=manifest.file_count,
            total_size=manifest.total_size,
        )
        manifest_path = temp_root / "manifest.v1"
        request_path = temp_root / "request.v1"
        signature_path = temp_root / "signature.bin"
        manifest_path.write_bytes(manifest.data)
        request_path.write_bytes(request.data)
        signature_path.write_bytes(
            sign(
                private_key,
                signed_job_bytes(request.data, manifest.data),
                backend=crypto_backend,
                openssl_executable=openssl_executable,
            )
        )
        os.replace(temp_root, final_root)
    except Exception:
        shutil.rmtree(temp_root, ignore_errors=True)
        raise
    return PreparedJob(
        root=final_root,
        package=final_root / "package",
        manifest_path=final_root / "manifest.v1",
        request_path=final_root / "request.v1",
        signature_path=final_root / "signature.bin",
        request=request,
        manifest=manifest,
        inspection=inspection,
    )


def load_prepared_job(path: os.PathLike[str] | str) -> PreparedJob:
    root = Path(path)
    manifest_path = root / "manifest.v1"
    request_path = root / "request.v1"
    signature_path = root / "signature.bin"
    package = root / "package"
    try:
        manifest = parse_manifest(manifest_path.read_bytes())
        request = parse_request(request_path.read_bytes())
        signature = signature_path.read_bytes()
    except OSError as exc:
        raise ProtocolError(f"prepared job is incomplete: {exc}") from exc
    if len(signature) != 64:
        raise ProtocolError("signature.bin must contain exactly 64 raw bytes")
    if root.name != request.job:
        raise ProtocolError("job directory name does not match request job id")
    if request.manifest_sha256 != manifest.sha256:
        raise ProtocolError("request manifest hash does not match manifest.v1")
    if request.file_count != manifest.file_count or request.total_size != manifest.total_size:
        raise ProtocolError("request manifest counts do not match manifest.v1")
    sfo_path = package / "sce_sys" / "param.sfo"
    try:
        metadata = parse_sfo(sfo_path.read_bytes())
    except OSError as exc:
        raise ProtocolError(f"prepared package is missing param.sfo: {exc}") from exc
    if metadata.title_id != request.title_id:
        raise ProtocolError("request TITLE_ID does not match package param.sfo")
    inspection = VpkInspection(
        title_id=metadata.title_id,
        content_id=metadata.content_id,
        entries=(),
        file_count=manifest.file_count,
        total_size=manifest.total_size,
    )
    return PreparedJob(root, package, manifest_path, request_path, signature_path, request, manifest, inspection)


def verify_prepared_job(
    path: os.PathLike[str] | str,
    public_key: os.PathLike[str] | str,
    *,
    crypto_backend: str = "auto",
    openssl_executable: str | None = None,
) -> PreparedJob:
    job = load_prepared_job(path)
    manifest_data = job.manifest_path.read_bytes()
    request_data = job.request_path.read_bytes()
    signature = job.signature_path.read_bytes()
    if not verify(
        public_key,
        signed_job_bytes(request_data, manifest_data),
        signature,
        backend=crypto_backend,
        openssl_executable=openssl_executable,
    ):
        raise ProtocolError("prepared job has an invalid Ed25519 signature")
    verify_manifest(job.package, manifest_data)
    return job
