"""Public-material-only provisioning helpers for the Vita auth gate."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import stat
import struct
import sys
import tempfile
from pathlib import Path

from vitadevdeploy.crypto import get_backend

from .auth_keys import JsonKeyStore
from .errors import AttachError, ProtocolError


_BUNDLE = struct.Struct(">4sIQQQQ32s")
_RECEIPT = struct.Struct(">4sIQQ32s")
_PUBLIC_DER_PREFIX = bytes.fromhex("302a300506032b6570032100")


def _u64(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "expected a nonzero unsigned 64-bit integer"
        ) from exc
    if not 1 <= parsed <= 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError(
            "expected a nonzero unsigned 64-bit integer"
        )
    return parsed


def _atomic_public_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def _read_exact_regular(path: Path, size: int, label: str) -> bytes:
    try:
        info = path.lstat()
    except OSError as exc:
        raise ProtocolError(f"could not inspect {label}: {exc}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise ProtocolError(f"{label} must be a regular file, not a link")
    if info.st_size != size:
        raise ProtocolError(f"{label} must be exactly {size} bytes")
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ProtocolError(f"could not read {label}: {exc}") from exc
    if len(data) != size:
        raise ProtocolError(f"{label} changed while it was read")
    return data


def _public_pem(raw: bytes) -> bytes:
    if len(raw) != 32 or not any(raw):
        raise ProtocolError("device receipt contains an invalid public key")
    encoded = base64.b64encode(_PUBLIC_DER_PREFIX + raw).decode("ascii")
    lines = [encoded[index : index + 64] for index in range(0, len(encoded), 64)]
    return (
        "-----BEGIN PUBLIC KEY-----\n"
        + "\n".join(lines)
        + "\n-----END PUBLIC KEY-----\n"
    ).encode("ascii")


def write_bundle(
    store: JsonKeyStore,
    output: Path,
    *,
    device_key_id: int,
    device_generation: int = 1,
) -> dict[str, object]:
    if device_generation != 1:
        raise ProtocolError("initial device generation must be exactly 1")
    local = store.local_identity()
    host_public = store._backend.public_raw(local.public_pem)
    if len(host_public) != 32 or not any(host_public):
        raise ProtocolError("host store returned an invalid public key")
    data = _BUNDLE.pack(
        b"VDAP",
        1,
        device_key_id,
        device_generation,
        local.key_id,
        local.generation,
        host_public,
    )
    _atomic_public_write(output, data)
    return {
        "schema": 1,
        "device_key_id": f"{device_key_id:016x}",
        "device_key_generation": device_generation,
        "host_key_id": f"{local.key_id:016x}",
        "host_key_generation": local.generation,
        "host_public_sha256": hashlib.sha256(host_public).hexdigest(),
        "bundle_sha256": hashlib.sha256(data).hexdigest(),
        "contains_private_material": False,
    }


def import_receipt(
    store: JsonKeyStore, receipt_path: Path
) -> dict[str, object]:
    data = _read_exact_regular(
        receipt_path, _RECEIPT.size, "device public receipt"
    )
    magic, schema, key_id, generation, public_raw = _RECEIPT.unpack(data)
    if magic != b"VDAR" or schema != 1 or key_id == 0 or generation == 0:
        raise ProtocolError("device public receipt is malformed")
    store.allow_peer(
        key_id=key_id,
        generation=generation,
        public_pem=_public_pem(public_raw),
    )
    return {
        "schema": schema,
        "device_key_id": f"{key_id:016x}",
        "device_key_generation": generation,
        "device_public_sha256": hashlib.sha256(public_raw).hexdigest(),
        "receipt_sha256": hashlib.sha256(data).hexdigest(),
        "contains_private_material": False,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="prepare or import public-only Vita auth provisioning data"
    )
    parser.add_argument(
        "--crypto-backend",
        choices=("auto", "cryptography", "openssl"),
        default="auto",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    bundle = commands.add_parser("make-bundle")
    bundle.add_argument("--key-store", required=True, type=Path)
    bundle.add_argument("--device-key-id", required=True, type=_u64)
    bundle.add_argument("--output", required=True, type=Path)
    receipt = commands.add_parser("import-receipt")
    receipt.add_argument("--key-store", required=True, type=Path)
    receipt.add_argument("--receipt", required=True, type=Path)
    revoke = commands.add_parser("revoke-peer")
    revoke.add_argument("--key-store", required=True, type=Path)
    revoke.add_argument("--key-id", required=True, type=_u64)
    revoke.add_argument("--generation", required=True, type=_u64)
    return parser


def run(args: argparse.Namespace) -> int:
    store = JsonKeyStore(
        args.key_store, get_backend(args.crypto_backend)
    )
    if args.command == "make-bundle":
        output = write_bundle(
            store,
            args.output,
            device_key_id=args.device_key_id,
            device_generation=1,
        )
    elif args.command == "import-receipt":
        output = import_receipt(store, args.receipt)
    else:
        store.revoke_peer(
            key_id=args.key_id, generation=args.generation
        )
        output = {
            "status": "revoked",
            "key_id": f"{args.key_id:016x}",
            "generation": args.generation,
        }
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (AttachError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
