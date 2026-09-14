#!/usr/bin/env python3
"""Configure and run the VitaDebugger VS Code demonstration workflow."""

from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TITLE_ID = "UVDBDEMO1"
CONFIG_FORMAT = 1
KILL_OK = "Killed."
KILL_NOT_RUNNING = "Error: cannot kill the app. Is the TITLEID correct?"
DEFAULT_PORTS = {"ftp": 1337, "companion": 1338, "gdb": 1234}

SAMPLE_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = SAMPLE_ROOT.parent.parent
VSCODE_DIR = SAMPLE_ROOT / ".vscode"
CONFIG_PATH = VSCODE_DIR / "vita.local.json"
LAUNCH_TEMPLATE_PATH = VSCODE_DIR / "launch.template.json"
LAUNCH_PATH = VSCODE_DIR / "launch.json"
CPP_TEMPLATE_PATH = VSCODE_DIR / "c_cpp_properties.template.json"
CPP_PATH = VSCODE_DIR / "c_cpp_properties.json"
BUILD_DIR = SAMPLE_ROOT / "build"
VPK_PATH = BUILD_DIR / "vscode-debug-demo.vpk"
ELF_PATH = BUILD_DIR / "vscode-debug-demo.elf"
IDENTITY_PATH = BUILD_DIR / "vscode-debug-demo.identity.json"
SYMBOL_SCRIPT_PATH = BUILD_DIR / "vscode-debug-demo-live.gdb"


class SessionError(RuntimeError):
    """A fail-closed setup or session error."""


@dataclass(frozen=True)
class DemoConfig:
    vita_ip: str
    ftp_port: int
    companion_port: int
    gdb_port: int
    vita_sdk_path: Path
    msys2_runtime_path: Path
    msys2_usr_bin_path: Path
    kubridge_dir: Path
    kubridge_lib_dir: Path
    deploy_private_key_path: Path
    openssl_path: Path

    def to_json(self) -> dict[str, Any]:
        return {
            "schemaVersion": CONFIG_FORMAT,
            "vitaIp": self.vita_ip,
            "ports": {
                "ftp": self.ftp_port,
                "companion": self.companion_port,
                "gdb": self.gdb_port,
            },
            "vitaSdkPath": str(self.vita_sdk_path),
            "msys2RuntimePath": str(self.msys2_runtime_path),
            "msys2UsrBinPath": str(self.msys2_usr_bin_path),
            "kubridgeDir": str(self.kubridge_dir),
            "kubridgeLibDir": str(self.kubridge_lib_dir),
            "deployPrivateKeyPath": str(self.deploy_private_key_path),
            "opensslPath": str(self.openssl_path),
        }


def canonical_ipv4(value: str) -> str:
    if not isinstance(value, str) or value != value.strip() or not value:
        raise SessionError("the Vita IP must be a canonical dotted-decimal IPv4 address")
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise SessionError(
            "the Vita IP must contain four decimal octets from 0 through 255"
        ) from exc
    if str(address) != value:
        raise SessionError("the Vita IP must use canonical dotted-decimal notation")
    return value


def checked_port(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
        raise SessionError(f"{label} must be an integer from 1 through 65535")
    return value


def configured_port(explicit: int | None, previous: Any, name: str) -> int:
    """Select an explicit, prior, or default port without treating zero as absent."""

    value = explicit if explicit is not None else previous
    if value is None:
        value = DEFAULT_PORTS[name]
    return checked_port(value, f"{name.upper()} port")


def checked_path(value: Any, label: str, *, directory: bool) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise SessionError(f"{label} must be a non-empty absolute path")
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise SessionError(f"{label} must be an absolute path: {path}")
    try:
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise SessionError(f"{label} was not found: {path}") from exc
    if directory and not resolved.is_dir():
        raise SessionError(f"{label} is not a directory: {resolved}")
    if not directory and not resolved.is_file():
        raise SessionError(f"{label} is not a file: {resolved}")
    return resolved


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise SessionError(f"{label} was not found: {path}")


def parse_config(data: Any) -> DemoConfig:
    if not isinstance(data, dict):
        raise SessionError("local configuration must be a JSON object")
    expected = {
        "schemaVersion",
        "vitaIp",
        "ports",
        "vitaSdkPath",
        "msys2RuntimePath",
        "msys2UsrBinPath",
        "kubridgeDir",
        "kubridgeLibDir",
        "deployPrivateKeyPath",
        "opensslPath",
    }
    if set(data) != expected:
        missing = sorted(expected - set(data))
        extra = sorted(set(data) - expected)
        raise SessionError(
            f"local configuration fields differ from schema; missing={missing}, extra={extra}"
        )
    if data["schemaVersion"] != CONFIG_FORMAT:
        raise SessionError(
            f"unsupported local configuration version {data['schemaVersion']!r}"
        )
    ports = data["ports"]
    if not isinstance(ports, dict) or set(ports) != set(DEFAULT_PORTS):
        raise SessionError("ports must contain exactly ftp, companion, and gdb")

    config = DemoConfig(
        vita_ip=canonical_ipv4(data["vitaIp"]),
        ftp_port=checked_port(ports["ftp"], "FTP port"),
        companion_port=checked_port(ports["companion"], "Companion port"),
        gdb_port=checked_port(ports["gdb"], "GDB port"),
        vita_sdk_path=checked_path(data["vitaSdkPath"], "VitaSDK path", directory=True),
        msys2_runtime_path=checked_path(
            data["msys2RuntimePath"], "MSYS2 runtime path", directory=True
        ),
        msys2_usr_bin_path=checked_path(
            data["msys2UsrBinPath"], "MSYS2 usr bin path", directory=True
        ),
        kubridge_dir=checked_path(data["kubridgeDir"], "Kubridge path", directory=True),
        kubridge_lib_dir=checked_path(
            data["kubridgeLibDir"], "Kubridge library path", directory=True
        ),
        deploy_private_key_path=checked_path(
            data["deployPrivateKeyPath"], "VitaDevDeploy private key", directory=False
        ),
        openssl_path=checked_path(data["opensslPath"], "OpenSSL executable", directory=False),
    )
    require_file(config.vita_sdk_path / "bin" / "arm-vita-eabi-gcc.exe", "VitaSDK GCC")
    require_file(config.vita_sdk_path / "bin" / "arm-vita-eabi-gdb.exe", "VitaSDK GDB")
    require_file(config.kubridge_dir / "kubridge.h", "Kubridge public header")
    require_file(config.kubridge_lib_dir / "libkubridge_stub.a", "Kubridge import library")
    return config


def load_config(path: Path = CONFIG_PATH) -> DemoConfig:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SessionError(
            f"local configuration is missing; run 'Vita: Configure device' ({path})"
        ) from exc
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise SessionError(f"local configuration is invalid JSON: {exc}") from exc
    return parse_config(data)


def atomic_write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(document, indent=2, ensure_ascii=True) + "\n"
    temporary_name = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", delete=False, dir=path.parent
        ) as handle:
            temporary_name = handle.name
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, path)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except OSError:
                pass


def replace_sentinels(value: Any, replacements: dict[str, str]) -> Any:
    if isinstance(value, list):
        return [replace_sentinels(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: replace_sentinels(item, replacements) for key, item in value.items()}
    if isinstance(value, str):
        if value in replacements:
            return replacements[value]
        if "__GENERATED_" in value:
            raise SessionError(f"unknown generated template sentinel {value!r}")
    return value


def load_json_template(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SessionError(f"cannot load template {path}: {exc}") from exc


def gdb_source_command(path: Path) -> str:
    # GDB's `source` command consumes the rest of its CLI line as the path. It
    # does not remove quote characters around that filename, so quoting a path
    # with spaces makes the quotes part of the filename. Escape only for the MI
    # string that encloses the complete CLI command.
    command = f"source {path.resolve().as_posix()}"
    escaped = command.replace("\\", "\\\\").replace('"', '\\"')
    return f'-interpreter-exec console "{escaped}"'


def create_launch_document(config: DemoConfig) -> dict[str, Any]:
    template = load_json_template(LAUNCH_TEMPLATE_PATH)
    return replace_sentinels(
        template,
        {
            "__GENERATED_GDB_PATH__": (
                config.vita_sdk_path / "bin" / "arm-vita-eabi-gdb.exe"
            ).as_posix(),
            "__GENERATED_SYMBOL_SOURCE_COMMAND__": gdb_source_command(
                SYMBOL_SCRIPT_PATH
            ),
        },
    )


def create_cpp_document(config: DemoConfig) -> dict[str, Any]:
    template = load_json_template(CPP_TEMPLATE_PATH)
    sdk_include = config.vita_sdk_path / "arm-vita-eabi" / "include"
    debug_screen = (
        config.vita_sdk_path
        / "share"
        / "gcc-arm-vita-eabi"
        / "samples"
        / "common"
    )
    return replace_sentinels(
        template,
        {
            "__GENERATED_REPO_ROOT__": REPO_ROOT.as_posix(),
            "__GENERATED_REPO_SRC__": (REPO_ROOT / "src").as_posix(),
            "__GENERATED_KUBRIDGE_DIR__": config.kubridge_dir.as_posix(),
            "__GENERATED_VITASDK_INCLUDE__": sdk_include.as_posix(),
            "__GENERATED_DEBUGSCREEN_INCLUDE__": debug_screen.as_posix(),
        },
    )


def write_editor_files(config: DemoConfig) -> None:
    atomic_write_json(LAUNCH_PATH, create_launch_document(config))
    atomic_write_json(CPP_PATH, create_cpp_document(config))


def validate_editor_files(config: DemoConfig) -> None:
    expected_launch = create_launch_document(config)
    expected_cpp = create_cpp_document(config)
    try:
        actual_launch = json.loads(LAUNCH_PATH.read_text(encoding="utf-8"))
        actual_cpp = json.loads(CPP_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SessionError(
            "generated VS Code files are missing or stale; run 'Vita: Configure device'"
        ) from exc
    if actual_launch != expected_launch or actual_cpp != expected_cpp:
        raise SessionError(
            "generated VS Code files do not match vita.local.json; run "
            "'Vita: Configure device' again"
        )
    serialized = json.dumps(actual_launch)
    if str(config.deploy_private_key_path) in serialized:
        raise SessionError("the generated launch configuration must not contain a private-key path")
    if "miDebuggerServerAddress" in serialized:
        raise SessionError("launch.json must not open a second GDB connection")


def first_existing(candidates: Iterable[Path], *, directory: bool) -> Path | None:
    for candidate in candidates:
        if str(candidate) in {"", "."}:
            continue
        try:
            resolved = candidate.expanduser().resolve(strict=True)
        except OSError:
            continue
        if (directory and resolved.is_dir()) or (not directory and resolved.is_file()):
            return resolved
    return None


def previous_values() -> dict[str, Any]:
    try:
        value = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def select_path(
    explicit: str | None,
    previous: Any,
    candidates: Iterable[Path],
    label: str,
    *,
    directory: bool,
) -> Path:
    if explicit:
        selected = Path(explicit)
    elif isinstance(previous, str) and previous:
        selected = Path(previous)
    else:
        detected = first_existing(candidates, directory=directory)
        if detected is not None:
            print(f"Detected {label}: {detected}")
            selected = detected
        else:
            if not sys.stdin.isatty():
                raise SessionError(
                    f"{label} was not detected; rerun configure with its command-line option"
                )
            entered = input(f"Enter {label}: ").strip()
            if not entered:
                raise SessionError(f"{label} is required")
            selected = Path(entered)
    return checked_path(str(selected), label, directory=directory)


def configure(args: argparse.Namespace) -> DemoConfig:
    old = previous_values()
    old_ports = old.get("ports") if isinstance(old.get("ports"), dict) else {}
    vita_ip_text = args.vita_ip
    if not vita_ip_text and isinstance(old.get("vitaIp"), str):
        vita_ip_text = old["vitaIp"]
    if not vita_ip_text:
        if not sys.stdin.isatty():
            raise SessionError("pass --vita-ip with the Vita's IPv4 address")
        vita_ip_text = input("Enter the PS Vita IPv4 address: ").strip()
    vita_ip = canonical_ipv4(vita_ip_text)

    vita_sdk = select_path(
        args.vita_sdk,
        old.get("vitaSdkPath"),
        [Path(os.environ.get("VITASDK", "")), Path("C:/vitasdk")],
        "VitaSDK path",
        directory=True,
    )
    msys_runtime = select_path(
        args.msys2_runtime,
        old.get("msys2RuntimePath"),
        [Path("C:/msys64/mingw64/bin")],
        "MSYS2 runtime path",
        directory=True,
    )
    msys_usr = select_path(
        args.msys2_usr,
        old.get("msys2UsrBinPath"),
        [Path("C:/msys64/usr/bin")],
        "MSYS2 usr bin path",
        directory=True,
    )
    kubridge = select_path(
        args.kubridge,
        old.get("kubridgeDir"),
        [REPO_ROOT.parent / "kubridge", REPO_ROOT.parent / "kubridge-review"],
        "Kubridge path",
        directory=True,
    )
    kubridge_lib = select_path(
        args.kubridge_lib,
        old.get("kubridgeLibDir"),
        [kubridge / "build-local", kubridge / "build"],
        "Kubridge library path",
        directory=True,
    )
    private_key = select_path(
        args.private_key,
        old.get("deployPrivateKeyPath"),
        [
            REPO_ROOT / "deploy" / "local" / "deploy_private.pem",
            REPO_ROOT.parent / "VitaDevDeploy" / "local" / "deploy_private.pem",
        ],
        "VitaDevDeploy private key",
        directory=False,
    )
    openssl = select_path(
        args.openssl,
        old.get("opensslPath"),
        [msys_usr / "openssl.exe", Path("C:/Program Files/OpenSSL-Win64/bin/openssl.exe")],
        "OpenSSL executable",
        directory=False,
    )

    config = parse_config(
        {
            "schemaVersion": CONFIG_FORMAT,
            "vitaIp": vita_ip,
            "ports": {
                "ftp": configured_port(args.ftp_port, old_ports.get("ftp"), "ftp"),
                "companion": configured_port(
                    args.companion_port, old_ports.get("companion"), "companion"
                ),
                "gdb": configured_port(args.gdb_port, old_ports.get("gdb"), "gdb"),
            },
            "vitaSdkPath": str(vita_sdk),
            "msys2RuntimePath": str(msys_runtime),
            "msys2UsrBinPath": str(msys_usr),
            "kubridgeDir": str(kubridge),
            "kubridgeLibDir": str(kubridge_lib),
            "deployPrivateKeyPath": str(private_key),
            "opensslPath": str(openssl),
        }
    )
    atomic_write_json(CONFIG_PATH, config.to_json())
    write_editor_files(config)
    print(f"Configured Vita {config.vita_ip}:{config.gdb_port}")
    print(f"Wrote ignored local profile: {CONFIG_PATH}")
    print("Press F5 and choose 'Vita: Build, deploy, and debug demo'.")
    return config


def display_command(command: list[str]) -> str:
    return subprocess.list2cmdline(command)


def run_checked(command: list[str], *, cwd: Path, display: str | None = None) -> None:
    print(f"> {display or display_command(command)}", flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise SessionError(
            f"command failed with exit code {completed.returncode}: {command[0]}"
        )


def powershell_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def msys_path(path: Path) -> str:
    normalized = path.resolve().as_posix()
    if len(normalized) >= 3 and normalized[1] == ":" and normalized[2] == "/":
        return f"/{normalized[0].lower()}/{normalized[3:]}"
    raise SessionError(f"the current Windows workflow requires a drive path: {path}")


def run_vita_environment(
    config: DemoConfig, arguments: list[str], *, display: str
) -> None:
    wrapper = REPO_ROOT / "tools" / "invoke-vita-env.ps1"
    require_file(wrapper, "validated VitaSDK environment wrapper")
    wrapper_arguments = [
        "-VitaSdkPath",
        str(config.vita_sdk_path),
        "-Msys2RuntimePath",
        str(config.msys2_runtime_path),
        "-Msys2UsrBinPath",
        str(config.msys2_usr_bin_path),
        *arguments,
    ]
    command_text = (
        "$ProgressPreference = 'SilentlyContinue'; & "
        + powershell_literal(str(wrapper))
        + " "
        + " ".join(powershell_literal(argument) for argument in wrapper_arguments)
    )
    encoded = base64.b64encode(command_text.encode("utf-16le")).decode("ascii")
    run_checked(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-OutputFormat",
            "Text",
            "-EncodedCommand",
            encoded,
        ],
        cwd=SAMPLE_ROOT,
        display=display,
    )


def validate_toolchain(config: DemoConfig) -> None:
    run_vita_environment(
        config,
        ["-ValidateOnly"],
        display="validate VitaSDK and MSYS2 compiler runtime",
    )


def build_demo(config: DemoConfig) -> None:
    validate_toolchain(config)
    run_vita_environment(
        config,
        [
            "--",
            "make.exe",
            "-B",
            "package",
            f"KUBRIDGE_DIR={config.kubridge_dir.as_posix()}",
            f"KUBRIDGE_LIB_DIR={config.kubridge_lib_dir.as_posix()}",
            f"GDB_PORT={config.gdb_port}",
            "UVDB_TOOL_PATH="
            + ":".join(
                (
                    msys_path(config.msys2_usr_bin_path),
                    msys_path(config.msys2_runtime_path),
                    msys_path(config.vita_sdk_path / "bin"),
                )
            ),
        ],
        display="build isolated VitaDebugger VS Code demo",
    )
    require_file(VPK_PATH, "debug demo VPK")
    require_file(ELF_PATH, "unstripped debug demo ELF")


def create_identity() -> None:
    run_checked(
        [
            sys.executable,
            str(REPO_ROOT / "tools" / "gdb_build_identity.py"),
            "--vpk",
            str(VPK_PATH),
            "--main-elf",
            str(ELF_PATH),
            "--title-id",
            TITLE_ID,
            "--output",
            str(IDENTITY_PATH),
            "--force",
        ],
        cwd=REPO_ROOT,
    )


def companion_bindings():
    deploy_host = REPO_ROOT / "deploy" / "host"
    if str(deploy_host) not in sys.path:
        sys.path.insert(0, str(deploy_host))
    try:
        from vitadevdeploy.companion import VitaCompanionClient
        from vitadevdeploy.errors import VitaDevDeployError
    except ImportError as exc:
        raise SessionError(
            "bundled VitaDevDeploy host package could not be imported"
        ) from exc
    return VitaCompanionClient, VitaDevDeployError


def companion_client(config: DemoConfig):
    client_type, _error_type = companion_bindings()
    return client_type(
        config.vita_ip, port=config.companion_port, timeout=10.0
    )


def classify_kill_response(response: str) -> str:
    if response == KILL_OK:
        return "stopped"
    if response == KILL_NOT_RUNNING:
        return "already stopped"
    raise SessionError(f"Vita Companion returned an unexpected stop response: {response!r}")


def stop_demo(config: DemoConfig, *, settle_before: bool) -> str:
    if settle_before:
        time.sleep(1.0)
    _client_type, error_type = companion_bindings()
    try:
        response = companion_client(config).kill(TITLE_ID)
    except error_type as exc:
        raise SessionError(
            f"could not stop {TITLE_ID} through Vita Companion: {exc}"
        ) from exc
    result = classify_kill_response(response)
    print(f"Vita demo is {result} ({TITLE_ID}).")
    if result == "stopped":
        time.sleep(1.0)
    return result


def deploy_demo(config: DemoConfig) -> None:
    run_checked(
        [
            sys.executable,
            "-m",
            "host.vitadevdeploy",
            "deploy",
            str(VPK_PATH),
            "--vita",
            config.vita_ip,
            "--private-key",
            str(config.deploy_private_key_path),
            "--ftp-port",
            str(config.ftp_port),
            "--command-port",
            str(config.companion_port),
            "--action",
            "install_launch",
            "--crypto-backend",
            "openssl",
            "--openssl",
            str(config.openssl_path),
        ],
        cwd=REPO_ROOT / "deploy",
    )


def create_live_symbols(config: DemoConfig) -> None:
    run_checked(
        [
            sys.executable,
            str(REPO_ROOT / "tools" / "gdb_symbols.py"),
            "--main-elf",
            str(ELF_PATH),
            "--host",
            config.vita_ip,
            "--port",
            str(config.gdb_port),
            "--timeout",
            "10",
            "--connect-wait",
            "20",
            "--search-root",
            str(BUILD_DIR),
            "--build-identity",
            str(IDENTITY_PATH),
            "--vpk",
            str(VPK_PATH),
            "--verify-installed",
            "--ftp-port",
            str(config.ftp_port),
            "--output",
            str(SYMBOL_SCRIPT_PATH),
        ],
        cwd=REPO_ROOT,
    )
    require_file(SYMBOL_SCRIPT_PATH, "ASLR-correct GDB symbol script")


def prepare_debug_session(config: DemoConfig) -> None:
    validate_editor_files(config)
    print(f"Preparing one-click debug session for Vita {config.vita_ip}...")
    build_demo(config)
    create_identity()
    stop_demo(config, settle_before=False)
    try:
        deploy_demo(config)
    except SessionError as exc:
        raise SessionError(
            f"deployment failed: {exc}. Keep the Vita awake at LiveArea, make "
            "sure Vita Companion is active, and close any unrelated running app"
        ) from exc
    try:
        create_live_symbols(config)
    except (SessionError, OSError) as exc:
        cleanup_message = "the exact demo title was stopped"
        try:
            stop_demo(config, settle_before=False)
        except SessionError as cleanup_error:
            cleanup_message = (
                f"automatic exact-title cleanup also failed: {cleanup_error}"
            )
        raise SessionError(
            f"live symbol preparation failed after deployment; {cleanup_message}: {exc}"
        ) from exc
    # The first symbol client has detached. Give the application-side persistent
    # service time to publish its fresh listener; do not consume it with a probe.
    time.sleep(1.0)
    print("Verified symbols are ready. VS Code can now own the GDB connection.")


def add_configure_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--vita-ip")
    parser.add_argument("--vita-sdk")
    parser.add_argument("--msys2-runtime")
    parser.add_argument("--msys2-usr")
    parser.add_argument("--kubridge")
    parser.add_argument("--kubridge-lib")
    parser.add_argument("--private-key")
    parser.add_argument("--openssl")
    parser.add_argument("--ftp-port", type=int)
    parser.add_argument("--companion-port", type=int)
    parser.add_argument("--gdb-port", type=int)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Configure, build, deploy, and stop the VitaDebugger VS Code demo"
    )
    subparsers = parser.add_subparsers(dest="action", required=True)
    configure_parser = subparsers.add_parser(
        "configure", help="write ignored per-user Vita/SDK settings and editor files"
    )
    add_configure_arguments(configure_parser)
    subparsers.add_parser("validate", help="validate the local profile and generated files")
    subparsers.add_parser("build", help="build the debug ELF and VPK")
    subparsers.add_parser(
        "prepare", help="build, deploy, launch, and capture verified live symbols"
    )
    subparsers.add_parser("stop", help=f"stop only {TITLE_ID} through Vita Companion")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.action == "configure":
            configure(args)
            return 0

        config = load_config()
        if args.action == "validate":
            validate_editor_files(config)
            validate_toolchain(config)
            print(
                f"Local setup is valid for Vita {config.vita_ip}:{config.gdb_port}."
            )
        elif args.action == "build":
            validate_editor_files(config)
            build_demo(config)
        elif args.action == "prepare":
            prepare_debug_session(config)
        elif args.action == "stop":
            stop_demo(config, settle_before=True)
        else:
            raise SessionError(f"unsupported action {args.action!r}")
    except (SessionError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
