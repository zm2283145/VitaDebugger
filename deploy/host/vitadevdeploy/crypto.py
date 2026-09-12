"""Optional Ed25519 providers with a clear dependency boundary."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path
from typing import Protocol

from .errors import CryptoUnavailableError, ProtocolError


class Ed25519Backend(Protocol):
    name: str

    def generate(self) -> tuple[bytes, bytes]: ...

    def sign(self, private_pem: bytes, data: bytes) -> bytes: ...

    def verify(self, public_pem: bytes, data: bytes, signature: bytes) -> bool: ...

    def public_raw(self, public_pem: bytes) -> bytes: ...


class CryptographyBackend:
    name = "cryptography"

    def __init__(self) -> None:
        try:
            from cryptography.hazmat.primitives import serialization
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
        except ImportError as exc:
            raise CryptoUnavailableError("Python package 'cryptography' is not installed") from exc
        self.serialization = serialization
        self.private_type = Ed25519PrivateKey
        self.public_type = Ed25519PublicKey

    def generate(self) -> tuple[bytes, bytes]:
        key = self.private_type.generate()
        private = key.private_bytes(
            encoding=self.serialization.Encoding.PEM,
            format=self.serialization.PrivateFormat.PKCS8,
            encryption_algorithm=self.serialization.NoEncryption(),
        )
        public = key.public_key().public_bytes(
            encoding=self.serialization.Encoding.PEM,
            format=self.serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        return private, public

    def sign(self, private_pem: bytes, data: bytes) -> bytes:
        try:
            key = self.serialization.load_pem_private_key(private_pem, password=None)
        except (TypeError, ValueError) as exc:
            raise ProtocolError("private key is not a readable unencrypted PEM key") from exc
        if not isinstance(key, self.private_type):
            raise ProtocolError("private key is not Ed25519")
        return key.sign(data)

    def verify(self, public_pem: bytes, data: bytes, signature: bytes) -> bool:
        try:
            key = self.serialization.load_pem_public_key(public_pem)
        except (TypeError, ValueError) as exc:
            raise ProtocolError("public key is not a readable PEM key") from exc
        if not isinstance(key, self.public_type):
            raise ProtocolError("public key is not Ed25519")
        try:
            key.verify(signature, data)
            return True
        except Exception as exc:
            invalid_signature = getattr(__import__("cryptography.exceptions", fromlist=["InvalidSignature"]), "InvalidSignature")
            if isinstance(exc, invalid_signature):
                return False
            raise

    def public_raw(self, public_pem: bytes) -> bytes:
        try:
            key = self.serialization.load_pem_public_key(public_pem)
        except (TypeError, ValueError) as exc:
            raise ProtocolError("public key is not a readable PEM key") from exc
        if not isinstance(key, self.public_type):
            raise ProtocolError("public key is not Ed25519")
        return key.public_bytes(
            encoding=self.serialization.Encoding.Raw,
            format=self.serialization.PublicFormat.Raw,
        )


class OpenSSLBackend:
    name = "openssl"

    def __init__(self, executable: str | None = None) -> None:
        self.executable = executable or os.environ.get("VITADEVDEPLOY_OPENSSL") or shutil.which("openssl") or ""
        if not self.executable:
            raise CryptoUnavailableError("OpenSSL was not found on PATH")
        probe = subprocess.run(
            [self.executable, "version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        if probe.returncode != 0:
            raise CryptoUnavailableError("OpenSSL could not be executed")

    def _run(self, arguments: list[str]) -> subprocess.CompletedProcess[bytes]:
        try:
            return subprocess.run(
                [self.executable, *arguments],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise CryptoUnavailableError(f"OpenSSL failed to run: {exc}") from exc

    def generate(self) -> tuple[bytes, bytes]:
        with tempfile.TemporaryDirectory(prefix="vitadevdeploy-key-") as directory:
            private_path = Path(directory) / "private.pem"
            public_path = Path(directory) / "public.pem"
            generated = self._run(["genpkey", "-algorithm", "ED25519", "-out", str(private_path)])
            if generated.returncode != 0:
                raise CryptoUnavailableError(f"OpenSSL cannot generate Ed25519 keys: {generated.stderr.decode(errors='replace').strip()}")
            exported = self._run(["pkey", "-in", str(private_path), "-pubout", "-out", str(public_path)])
            if exported.returncode != 0:
                raise CryptoUnavailableError(f"OpenSSL cannot export an Ed25519 public key: {exported.stderr.decode(errors='replace').strip()}")
            return private_path.read_bytes(), public_path.read_bytes()

    def sign(self, private_pem: bytes, data: bytes) -> bytes:
        with tempfile.TemporaryDirectory(prefix="vitadevdeploy-sign-") as directory:
            root = Path(directory)
            private_path = root / "private.pem"
            data_path = root / "data.bin"
            signature_path = root / "signature.bin"
            private_path.write_bytes(private_pem)
            data_path.write_bytes(data)
            result = self._run([
                "pkeyutl", "-sign", "-rawin", "-inkey", str(private_path),
                "-in", str(data_path), "-out", str(signature_path),
            ])
            if result.returncode != 0:
                raise ProtocolError(f"OpenSSL Ed25519 signing failed: {result.stderr.decode(errors='replace').strip()}")
            signature = signature_path.read_bytes()
        if len(signature) != 64:
            raise ProtocolError("OpenSSL returned an invalid Ed25519 signature length")
        return signature

    def verify(self, public_pem: bytes, data: bytes, signature: bytes) -> bool:
        if len(signature) != 64:
            return False
        with tempfile.TemporaryDirectory(prefix="vitadevdeploy-verify-") as directory:
            root = Path(directory)
            public_path = root / "public.pem"
            data_path = root / "data.bin"
            signature_path = root / "signature.bin"
            public_path.write_bytes(public_pem)
            data_path.write_bytes(data)
            signature_path.write_bytes(signature)
            result = self._run([
                "pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", str(public_path),
                "-sigfile", str(signature_path), "-in", str(data_path),
            ])
            return result.returncode == 0

    def public_raw(self, public_pem: bytes) -> bytes:
        with tempfile.TemporaryDirectory(prefix="vitadevdeploy-public-") as directory:
            root = Path(directory)
            public_path = root / "public.pem"
            der_path = root / "public.der"
            public_path.write_bytes(public_pem)
            result = self._run([
                "pkey", "-pubin", "-in", str(public_path), "-outform", "DER", "-out", str(der_path),
            ])
            if result.returncode != 0:
                raise ProtocolError(f"OpenSSL public-key export failed: {result.stderr.decode(errors='replace').strip()}")
            der = der_path.read_bytes()
        prefix = bytes.fromhex("302a300506032b6570032100")
        if len(der) != len(prefix) + 32 or not der.startswith(prefix):
            raise ProtocolError("OpenSSL key is not an Ed25519 public key")
        return der[len(prefix) :]


def get_backend(name: str = "auto", *, openssl_executable: str | None = None) -> Ed25519Backend:
    if name not in {"auto", "cryptography", "openssl"}:
        raise CryptoUnavailableError(f"unknown Ed25519 backend: {name}")
    errors: list[str] = []
    if name in {"auto", "cryptography"}:
        try:
            return CryptographyBackend()
        except CryptoUnavailableError as exc:
            errors.append(str(exc))
            if name == "cryptography":
                raise
    if name in {"auto", "openssl"}:
        try:
            return OpenSSLBackend(openssl_executable)
        except CryptoUnavailableError as exc:
            errors.append(str(exc))
            if name == "openssl":
                raise
    detail = "; ".join(errors)
    raise CryptoUnavailableError(
        "Ed25519 support is unavailable. Install with 'py -m pip install cryptography' "
        "or place an Ed25519-capable OpenSSL on PATH. " + detail
    )


def _atomic_write(path: Path, data: bytes, *, private: bool, force: bool) -> None:
    if path.exists() and not force:
        raise ProtocolError(f"refusing to overwrite existing key: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    try:
        if private:
            os.chmod(temporary, stat.S_IRUSR | stat.S_IWUSR)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        if private:
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def generate_keypair(
    private_path: os.PathLike[str] | str,
    public_path: os.PathLike[str] | str,
    *,
    backend: str = "auto",
    openssl_executable: str | None = None,
    force: bool = False,
) -> str:
    selected = get_backend(backend, openssl_executable=openssl_executable)
    private, public = selected.generate()
    private_target = Path(private_path)
    public_target = Path(public_path)
    if private_target.resolve() == public_target.resolve():
        raise ProtocolError("private and public key paths must differ")
    if not force and (private_target.exists() or public_target.exists()):
        raise ProtocolError("refusing to overwrite an existing key")
    _atomic_write(private_target, private, private=True, force=force)
    try:
        _atomic_write(public_target, public, private=False, force=force)
    except Exception:
        try:
            private_target.unlink()
        except OSError:
            pass
        raise
    return selected.name


def sign(
    private_path: os.PathLike[str] | str,
    data: bytes,
    *,
    backend: str = "auto",
    openssl_executable: str | None = None,
) -> bytes:
    try:
        private = Path(private_path).read_bytes()
    except OSError as exc:
        raise ProtocolError(f"could not read private key: {exc}") from exc
    signature = get_backend(backend, openssl_executable=openssl_executable).sign(private, data)
    if len(signature) != 64:
        raise ProtocolError("Ed25519 signature is not 64 bytes")
    return signature


def verify(
    public_path: os.PathLike[str] | str,
    data: bytes,
    signature: bytes,
    *,
    backend: str = "auto",
    openssl_executable: str | None = None,
) -> bool:
    try:
        public = Path(public_path).read_bytes()
    except OSError as exc:
        raise ProtocolError(f"could not read public key: {exc}") from exc
    return get_backend(backend, openssl_executable=openssl_executable).verify(public, data, signature)


def public_key_raw(
    public_path: os.PathLike[str] | str,
    *,
    backend: str = "auto",
    openssl_executable: str | None = None,
) -> bytes:
    try:
        public = Path(public_path).read_bytes()
    except OSError as exc:
        raise ProtocolError(f"could not read public key: {exc}") from exc
    raw = get_backend(backend, openssl_executable=openssl_executable).public_raw(public)
    if len(raw) != 32:
        raise ProtocolError("Ed25519 public key is not 32 bytes")
    return raw


def public_key_header(raw: bytes) -> bytes:
    if len(raw) != 32:
        raise ProtocolError("Ed25519 public key is not 32 bytes")
    rows = [
        "#ifndef VITADEVDEPLOY_PUBLIC_KEY_H",
        "#define VITADEVDEPLOY_PUBLIC_KEY_H",
        "",
        "static const unsigned char vitadevdeploy_public_key[32] = {",
    ]
    for offset in range(0, len(raw), 8):
        chunk = ", ".join(f"0x{value:02x}" for value in raw[offset : offset + 8])
        rows.append(f"    {chunk},")
    rows.extend(("};", "", "#endif", ""))
    return "\n".join(rows).encode("ascii")


def export_public_key(
    public_path: os.PathLike[str] | str,
    *,
    raw_path: os.PathLike[str] | str | None = None,
    header_path: os.PathLike[str] | str | None = None,
    backend: str = "auto",
    openssl_executable: str | None = None,
    force: bool = False,
) -> bytes:
    raw = public_key_raw(
        public_path,
        backend=backend,
        openssl_executable=openssl_executable,
    )
    if raw_path is None and header_path is None:
        raise ProtocolError("select at least one raw public-key output")
    targets = [Path(path) for path in (raw_path, header_path) if path is not None]
    if len({target.resolve() for target in targets}) != len(targets):
        raise ProtocolError("public-key output paths must differ")
    if not force and any(target.exists() for target in targets):
        raise ProtocolError("refusing to overwrite an existing public-key output")
    if raw_path is not None:
        _atomic_write(Path(raw_path), raw, private=False, force=force)
    if header_path is not None:
        _atomic_write(Path(header_path), public_key_header(raw), private=False, force=force)
    return raw
