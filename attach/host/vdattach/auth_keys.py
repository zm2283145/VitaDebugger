"""Fail-closed key storage for authenticated attach protocol version 2."""

from __future__ import annotations

import hmac
import json
import os
import stat
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from .errors import ProtocolError


KEY_ID_BYTES = 8
PUBLIC_KEY_BYTES = 32
SIGNATURE_BYTES = 64
MAX_KEY_FILE_BYTES = 16 * 1024
MAX_STATE_FILE_BYTES = 64 * 1024


class Ed25519Backend(Protocol):
    name: str

    def generate(self) -> tuple[bytes, bytes]: ...

    def sign(self, private_pem: bytes, data: bytes) -> bytes: ...

    def verify(
        self, public_pem: bytes, data: bytes, signature: bytes
    ) -> bool: ...

    def public_raw(self, public_pem: bytes) -> bytes: ...


@dataclass(frozen=True)
class LocalIdentity:
    key_id: int
    generation: int
    private_pem: bytes
    public_pem: bytes


@dataclass(frozen=True)
class TrustedPeer:
    key_id: int
    generation: int
    public_pem: bytes


class KeyStore(Protocol):
    def local_identity(self) -> LocalIdentity: ...

    def trusted_peer(self, key_id: int, generation: int) -> TrustedPeer: ...

    def sign_local(self, data: bytes) -> bytes: ...

    def verify_peer(
        self, key_id: int, generation: int, data: bytes, signature: bytes
    ) -> bool: ...


class UnavailableKeyStore:
    """Default Vita boundary: authentication is unavailable, never bypassed."""

    def _unavailable(self) -> None:
        raise ProtocolError("authenticated attach key storage is unavailable")

    def local_identity(self) -> LocalIdentity:
        self._unavailable()

    def trusted_peer(self, key_id: int, generation: int) -> TrustedPeer:
        del key_id, generation
        self._unavailable()

    def sign_local(self, data: bytes) -> bytes:
        del data
        self._unavailable()

    def verify_peer(
        self, key_id: int, generation: int, data: bytes, signature: bytes
    ) -> bool:
        del key_id, generation, data, signature
        self._unavailable()


def _checked_key_id(value: int, label: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 1 <= value <= 0xFFFFFFFFFFFFFFFF
    ):
        raise ProtocolError(f"{label} must be a nonzero unsigned 64-bit integer")
    return value


def _checked_generation(value: int, label: str) -> int:
    return _checked_key_id(value, label)


def _key_id_text(value: int) -> str:
    return f"{_checked_key_id(value, 'key ID'):016x}"


def _parse_key_id(value: object, label: str) -> int:
    if (
        not isinstance(value, str)
        or len(value) != 16
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ProtocolError(f"{label} must be 16 lowercase hexadecimal digits")
    return _checked_key_id(int(value, 16), label)


def _read_bounded_regular(path: Path, label: str, maximum: int) -> bytes:
    try:
        info = path.lstat()
    except OSError as exc:
        raise ProtocolError(f"could not inspect {label}: {exc}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise ProtocolError(f"{label} must be a regular file, not a link")
    if info.st_size <= 0 or info.st_size > maximum:
        raise ProtocolError(f"{label} has an invalid size")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise ProtocolError(f"could not read {label}: {exc}") from exc


def _atomic_write(path: Path, data: bytes, *, private: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}-", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        if private:
            os.chmod(temporary, stat.S_IRUSR | stat.S_IWUSR)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        if private:
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    except Exception:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


class JsonKeyStore:
    """Atomic host key store with explicit active/revoked peer records."""

    _STATE_NAME = "state.json"

    def __init__(self, root: os.PathLike[str] | str, backend: Ed25519Backend):
        self._root = Path(root)
        self._backend = backend

    @property
    def root(self) -> Path:
        return self._root

    def _state_path(self) -> Path:
        return self._root / self._STATE_NAME

    def _load_state(self) -> dict[str, object]:
        data = _read_bounded_regular(
            self._state_path(), "attach key state", MAX_STATE_FILE_BYTES
        )
        try:
            state = json.loads(data.decode("ascii"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ProtocolError("attach key state is not canonical JSON") from exc
        if not isinstance(state, dict) or state.get("schema") != 1:
            raise ProtocolError("attach key state schema is unsupported")
        if set(state) != {"schema", "revision", "local", "peers"}:
            raise ProtocolError("attach key state has unknown or missing fields")
        revision = state["revision"]
        if (
            isinstance(revision, bool)
            or not isinstance(revision, int)
            or revision < 1
        ):
            raise ProtocolError("attach key state revision is invalid")
        if not isinstance(state["local"], dict) or not isinstance(
            state["peers"], list
        ):
            raise ProtocolError("attach key state structure is invalid")
        seen: set[tuple[int, int]] = set()
        for record in state["peers"]:
            if not isinstance(record, dict) or set(record) != {
                "key_id",
                "generation",
                "public",
                "status",
            }:
                raise ProtocolError("peer key record is invalid")
            key_id = _parse_key_id(record["key_id"], "peer key ID")
            generation = _checked_generation(
                record["generation"], "peer key generation"
            )
            self._key_path(record["public"], "peer public key")
            if record["status"] not in {"active", "revoked"}:
                raise ProtocolError("peer key status is invalid")
            identity = (key_id, generation)
            if identity in seen:
                raise ProtocolError("peer key record is duplicated")
            seen.add(identity)
        return state

    def _write_state(self, state: dict[str, object]) -> None:
        encoded = (
            json.dumps(
                state, sort_keys=True, separators=(",", ":"), ensure_ascii=True
            )
            + "\n"
        ).encode("ascii")
        if len(encoded) > MAX_STATE_FILE_BYTES:
            raise ProtocolError("attach key state exceeds its fixed size limit")
        _atomic_write(self._state_path(), encoded, private=True)

    def _key_path(self, name: object, label: str) -> Path:
        if (
            not isinstance(name, str)
            or not name
            or Path(name).name != name
            or name in {".", ".."}
        ):
            raise ProtocolError(f"{label} has an invalid file name")
        return self._root / name

    def provision(
        self,
        *,
        local_key_id: int,
        local_generation: int = 1,
    ) -> LocalIdentity:
        if self._state_path().exists():
            raise ProtocolError("attach key store is already provisioned")
        key_id = _checked_key_id(local_key_id, "local key ID")
        generation = _checked_generation(
            local_generation, "local key generation"
        )
        private_pem, public_pem = self._backend.generate()
        private_name = f"local-{key_id:016x}-{generation}.private.pem"
        public_name = f"local-{key_id:016x}-{generation}.public.pem"
        self._root.mkdir(parents=True, exist_ok=True)
        _atomic_write(self._root / private_name, private_pem, private=True)
        try:
            _atomic_write(self._root / public_name, public_pem, private=False)
            self._write_state(
                {
                    "schema": 1,
                    "revision": 1,
                    "local": {
                        "key_id": _key_id_text(key_id),
                        "generation": generation,
                        "private": private_name,
                        "public": public_name,
                    },
                    "peers": [],
                }
            )
        except Exception:
            for name in (private_name, public_name):
                try:
                    (self._root / name).unlink()
                except OSError:
                    pass
            raise
        return self.local_identity()

    def rotate_local(self, *, new_key_id: int) -> LocalIdentity:
        state = self._load_state()
        old_local = self._parse_local(state["local"])
        key_id = _checked_key_id(new_key_id, "new local key ID")
        if hmac.compare_digest(
            key_id.to_bytes(KEY_ID_BYTES, "big"),
            old_local.key_id.to_bytes(KEY_ID_BYTES, "big"),
        ):
            raise ProtocolError("rotation requires a different local key ID")
        generation = old_local.generation + 1
        if generation > 0xFFFFFFFFFFFFFFFF:
            raise ProtocolError("local key generation is exhausted")
        private_pem, public_pem = self._backend.generate()
        private_name = f"local-{key_id:016x}-{generation}.private.pem"
        public_name = f"local-{key_id:016x}-{generation}.public.pem"
        _atomic_write(self._root / private_name, private_pem, private=True)
        try:
            _atomic_write(self._root / public_name, public_pem, private=False)
            local = state["local"]
            assert isinstance(local, dict)
            old_names = (local["private"], local["public"])
            state["revision"] = int(state["revision"]) + 1
            state["local"] = {
                "key_id": _key_id_text(key_id),
                "generation": generation,
                "private": private_name,
                "public": public_name,
            }
            self._write_state(state)
        except Exception:
            for name in (private_name, public_name):
                try:
                    (self._root / name).unlink()
                except OSError:
                    pass
            raise
        for name in old_names:
            path = self._key_path(name, "old local key")
            try:
                path.unlink()
            except OSError as exc:
                raise ProtocolError(
                    "rotation activated the new key but could not remove "
                    f"retired key file {path}"
                ) from exc
        return self.local_identity()

    def allow_peer(
        self,
        *,
        key_id: int,
        generation: int,
        public_pem: bytes,
    ) -> TrustedPeer:
        state = self._load_state()
        checked_id = _checked_key_id(key_id, "peer key ID")
        checked_generation = _checked_generation(
            generation, "peer key generation"
        )
        if (
            not isinstance(public_pem, bytes)
            or not public_pem
            or len(public_pem) > MAX_KEY_FILE_BYTES
        ):
            raise ProtocolError("peer public key has an invalid size")
        raw = self._backend.public_raw(public_pem)
        if len(raw) != PUBLIC_KEY_BYTES:
            raise ProtocolError("peer public key is not Ed25519")
        peers = state["peers"]
        assert isinstance(peers, list)
        for record in peers:
            if not isinstance(record, dict):
                raise ProtocolError("peer key record is invalid")
            existing_id = _parse_key_id(record.get("key_id"), "peer key ID")
            if hmac.compare_digest(
                existing_id.to_bytes(KEY_ID_BYTES, "big"),
                checked_id.to_bytes(KEY_ID_BYTES, "big"),
            ):
                if record.get("generation") == checked_generation:
                    raise ProtocolError(
                        "peer key ID and generation are already recorded"
                    )
                if record.get("status") == "active":
                    raise ProtocolError("peer key ID is already active")
        name = f"peer-{checked_id:016x}-{checked_generation}.public.pem"
        _atomic_write(self._root / name, public_pem, private=False)
        peers.append(
            {
                "key_id": _key_id_text(checked_id),
                "generation": checked_generation,
                "public": name,
                "status": "active",
            }
        )
        state["revision"] = int(state["revision"]) + 1
        try:
            self._write_state(state)
        except Exception:
            try:
                (self._root / name).unlink()
            except OSError:
                pass
            raise
        return self.trusted_peer(checked_id, checked_generation)

    def revoke_peer(self, *, key_id: int, generation: int) -> None:
        state = self._load_state()
        checked_id = _checked_key_id(key_id, "peer key ID")
        checked_generation = _checked_generation(
            generation, "peer key generation"
        )
        peers = state["peers"]
        assert isinstance(peers, list)
        matched = False
        for record in peers:
            if not isinstance(record, dict):
                raise ProtocolError("peer key record is invalid")
            record_id = _parse_key_id(record.get("key_id"), "peer key ID")
            record_generation = record.get("generation")
            if (
                hmac.compare_digest(
                    record_id.to_bytes(KEY_ID_BYTES, "big"),
                    checked_id.to_bytes(KEY_ID_BYTES, "big"),
                )
                and record_generation == checked_generation
                and record.get("status") == "active"
            ):
                record["status"] = "revoked"
                matched = True
        if not matched:
            raise ProtocolError("active peer key was not found")
        state["revision"] = int(state["revision"]) + 1
        self._write_state(state)

    def _parse_local(self, value: object) -> LocalIdentity:
        if not isinstance(value, dict) or set(value) != {
            "key_id",
            "generation",
            "private",
            "public",
        }:
            raise ProtocolError("local key record is invalid")
        key_id = _parse_key_id(value["key_id"], "local key ID")
        generation = _checked_generation(
            value["generation"], "local key generation"
        )
        private_pem = _read_bounded_regular(
            self._key_path(value["private"], "local private key"),
            "local private key",
            MAX_KEY_FILE_BYTES,
        )
        public_pem = _read_bounded_regular(
            self._key_path(value["public"], "local public key"),
            "local public key",
            MAX_KEY_FILE_BYTES,
        )
        if len(self._backend.public_raw(public_pem)) != PUBLIC_KEY_BYTES:
            raise ProtocolError("local public key is not Ed25519")
        return LocalIdentity(key_id, generation, private_pem, public_pem)

    def local_identity(self) -> LocalIdentity:
        return self._parse_local(self._load_state()["local"])

    def trusted_peer(self, key_id: int, generation: int) -> TrustedPeer:
        checked_id = _checked_key_id(key_id, "peer key ID")
        checked_generation = _checked_generation(
            generation, "peer key generation"
        )
        peers = self._load_state()["peers"]
        assert isinstance(peers, list)
        for record in peers:
            if not isinstance(record, dict) or set(record) != {
                "key_id",
                "generation",
                "public",
                "status",
            }:
                raise ProtocolError("peer key record is invalid")
            record_id = _parse_key_id(record["key_id"], "peer key ID")
            if (
                hmac.compare_digest(
                    record_id.to_bytes(KEY_ID_BYTES, "big"),
                    checked_id.to_bytes(KEY_ID_BYTES, "big"),
                )
                and record["generation"] == checked_generation
            ):
                if record["status"] != "active":
                    raise ProtocolError("peer key is revoked")
                public_pem = _read_bounded_regular(
                    self._key_path(record["public"], "peer public key"),
                    "peer public key",
                    MAX_KEY_FILE_BYTES,
                )
                if len(self._backend.public_raw(public_pem)) != PUBLIC_KEY_BYTES:
                    raise ProtocolError("peer public key is not Ed25519")
                return TrustedPeer(
                    checked_id, checked_generation, public_pem
                )
        raise ProtocolError("peer key is not in the explicit allowlist")

    def sign_local(self, data: bytes) -> bytes:
        if not isinstance(data, bytes):
            raise ProtocolError("signed transcript must be bytes")
        signature = self._backend.sign(
            self.local_identity().private_pem, data
        )
        if len(signature) != SIGNATURE_BYTES:
            raise ProtocolError("Ed25519 provider returned a bad signature")
        return signature

    def verify_peer(
        self, key_id: int, generation: int, data: bytes, signature: bytes
    ) -> bool:
        if not isinstance(data, bytes) or not isinstance(signature, bytes):
            raise ProtocolError("verification inputs must be bytes")
        if len(signature) != SIGNATURE_BYTES:
            return False
        peer = self.trusted_peer(key_id, generation)
        return self._backend.verify(peer.public_pem, data, signature)
