from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SAMPLE = ROOT / "examples" / "vscode-debug-demo"
sys.path.insert(0, str(SAMPLE / "tools"))

import session  # noqa: E402


def sample_config() -> session.DemoConfig:
    return session.DemoConfig(
        vita_ip="192.0.2.10",
        ftp_port=1337,
        companion_port=1338,
        gdb_port=1234,
        vita_sdk_path=Path("C:/vitasdk"),
        msys2_runtime_path=Path("C:/msys64/mingw64/bin"),
        msys2_usr_bin_path=Path("C:/msys64/usr/bin"),
        kubridge_dir=Path("C:/source/kubridge"),
        kubridge_lib_dir=Path("C:/source/kubridge/build-local"),
        deploy_private_key_path=Path("C:/private/deploy_private.pem"),
        openssl_path=Path("C:/msys64/usr/bin/openssl.exe"),
    )


class VitaDemoSessionTests(unittest.TestCase):
    def test_ipv4_is_configurable_but_strictly_canonical(self):
        self.assertEqual(session.canonical_ipv4("192.0.2.25"), "192.0.2.25")
        for rejected in (
            "vita.local",
            "10.1.1",
            "10.1.1.256",
            "0192.0.2.25",
            " 192.0.2.25",
            "::1",
            "",
        ):
            with self.subTest(rejected=rejected):
                with self.assertRaises(session.SessionError):
                    session.canonical_ipv4(rejected)

    def test_explicit_zero_port_is_rejected_instead_of_using_a_default(self):
        with self.assertRaises(session.SessionError):
            session.configured_port(0, 1337, "ftp")
        self.assertEqual(session.configured_port(None, 1444, "ftp"), 1444)
        self.assertEqual(session.configured_port(None, None, "gdb"), 1234)

    def test_generated_launch_uses_one_symbol_owned_connection(self):
        config = sample_config()
        document = session.create_launch_document(config)
        launch = document["configurations"][0]
        self.assertEqual(
            launch["miDebuggerPath"], "C:/vitasdk/bin/arm-vita-eabi-gdb.exe"
        )
        self.assertEqual(launch["targetArchitecture"], "arm")
        self.assertEqual(launch["unknownBreakpointHandling"], "stop")
        self.assertEqual(
            launch["preLaunchTask"], "Vita: Build, deploy, and prepare debug"
        )
        self.assertEqual(launch["postDebugTask"], "Vita: Stop demo")
        self.assertNotIn("hardwareBreakpoints", launch)
        serialized = json.dumps(document)
        self.assertNotIn("miDebuggerServerAddress", serialized)
        self.assertNotIn(str(config.deploy_private_key_path), serialized)
        self.assertIn("vscode-debug-demo-live.gdb", serialized)
        self.assertIn("vscode_demo_breakpoint", serialized)
        source = launch["customLaunchSetupCommands"][0]["text"]
        self.assertIn(
            f"source {session.SYMBOL_SCRIPT_PATH.resolve().as_posix()}", source
        )
        self.assertNotIn('source \\"', source)

    def test_intellisense_never_directly_probes_the_cross_compiler(self):
        document = session.create_cpp_document(sample_config())
        config = document["configurations"][0]
        self.assertEqual(config["compilerPath"], "")
        self.assertEqual(config["intelliSenseMode"], "windows-gcc-arm")
        self.assertIn("C:/vitasdk/arm-vita-eabi/include", config["includePath"])

    def test_stop_accepts_only_exact_known_companion_responses(self):
        self.assertEqual(session.classify_kill_response(session.KILL_OK), "stopped")
        self.assertEqual(
            session.classify_kill_response(session.KILL_NOT_RUNNING),
            "already stopped",
        )
        for rejected in ("", "Apps destroyed.", "Killed.\n", "Error: rebooting"):
            with self.subTest(rejected=rejected):
                with self.assertRaises(session.SessionError):
                    session.classify_kill_response(rejected)

    def test_prepare_sequence_is_serial_and_ends_with_symbol_snapshot(self):
        calls = []
        config = sample_config()
        patches = (
            mock.patch.object(
                session, "validate_editor_files", side_effect=lambda _cfg: calls.append("editor")
            ),
            mock.patch.object(
                session, "build_demo", side_effect=lambda _cfg: calls.append("build")
            ),
            mock.patch.object(
                session, "create_identity", side_effect=lambda: calls.append("identity")
            ),
            mock.patch.object(
                session,
                "stop_demo",
                side_effect=lambda _cfg, settle_before: calls.append(
                    f"stop:{settle_before}"
                ),
            ),
            mock.patch.object(
                session, "deploy_demo", side_effect=lambda _cfg: calls.append("deploy")
            ),
            mock.patch.object(
                session,
                "create_live_symbols",
                side_effect=lambda _cfg: calls.append("symbols"),
            ),
            mock.patch.object(session.time, "sleep", return_value=None),
        )
        with patches[0], patches[1], patches[2], patches[3], patches[4], patches[5], patches[6]:
            session.prepare_debug_session(config)
        self.assertEqual(
            calls,
            ["editor", "build", "identity", "stop:False", "deploy", "symbols"],
        )

    def test_prepare_failure_after_deploy_stops_only_the_demo(self):
        calls = []
        config = sample_config()
        with (
            mock.patch.object(session, "validate_editor_files"),
            mock.patch.object(session, "build_demo"),
            mock.patch.object(session, "create_identity"),
            mock.patch.object(
                session,
                "stop_demo",
                side_effect=lambda _cfg, settle_before: calls.append(settle_before)
                or "stopped",
            ),
            mock.patch.object(session, "deploy_demo"),
            mock.patch.object(
                session,
                "create_live_symbols",
                side_effect=session.SessionError("snapshot failed"),
            ),
        ):
            with self.assertRaisesRegex(
                session.SessionError, "exact demo title was stopped"
            ):
                session.prepare_debug_session(config)
        self.assertEqual(calls, [False, False])

    def test_symbol_snapshot_uses_configured_ip_without_a_probe(self):
        commands = []
        with (
            mock.patch.object(
                session,
                "run_checked",
                side_effect=lambda command, cwd: commands.append((command, cwd)),
            ),
            mock.patch.object(session, "require_file"),
        ):
            session.create_live_symbols(sample_config())
        command, _cwd = commands[0]
        self.assertEqual(command[command.index("--host") + 1], "192.0.2.10")
        self.assertEqual(command[command.index("--connect-wait") + 1], "20")
        self.assertNotIn("Test-NetConnection", command)

    def test_build_compiles_the_configured_gdb_port_into_the_demo(self):
        commands = []
        config = sample_config()
        config = session.DemoConfig(**{**config.__dict__, "gdb_port": 4321})
        with (
            mock.patch.object(session, "validate_toolchain"),
            mock.patch.object(
                session,
                "run_vita_environment",
                side_effect=lambda _cfg, arguments, display: commands.append(arguments),
            ),
            mock.patch.object(session, "require_file"),
        ):
            session.build_demo(config)
        self.assertIn("-B", commands[0])
        self.assertIn("GDB_PORT=4321", commands[0])

    def test_tracked_example_uses_an_ip_placeholder(self):
        example = json.loads(
            (SAMPLE / "vita-debug.example.json").read_text(encoding="utf-8")
        )
        self.assertEqual(example["vitaIp"], "YOUR_VITA_IP")


if __name__ == "__main__":
    unittest.main()
