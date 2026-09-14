import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import gdb_symbols as symbols  # noqa: E402
from tests.host.test_gdb_symbols import MockServer, make_elf  # noqa: E402


def state(
    modules: list[tuple[str, list[int], str | None]],
    *,
    script: str = "old script\n",
    vpk: str = "1" * 64,
) -> dict:
    return symbols.validate_symbol_state({
        "format": symbols.SYMBOL_STATE_FORMAT,
        "build": {
            "verification": "local",
            "title_id": "SLRS00001",
            "vpk_sha256": vpk,
            "identity_sha256": "8" * 64,
        },
        "main": {"name": "main", "elf_sha256": "2" * 64},
        "offsets": {"style": "segments", "text": 0x81000000, "data": 0x81010000},
        "modules": [
            {
                "name": name,
                "segments": addresses,
                "symbol_sha256": digest,
                "match_reason": "main executable" if name == "main" else (
                    "embedded Vita module name" if digest is not None else None
                ),
            }
            for name, addresses, digest in modules
        ],
        "script_sha256": hashlib.sha256(script.encode()).hexdigest(),
    })


class SymbolRefreshTests(unittest.TestCase):
    def test_reports_added_removed_rebased_and_symbol_changes(self):
        old = state([
            ("main", [0x81000000, 0x81010000], "2" * 64),
            ("Removed", [0x82000000], None),
            ("Moved", [0x83000000], "3" * 64),
            ("Rematched", [0x84000000], None),
        ])
        new = state([
            ("main", [0x81000000, 0x81010000], "2" * 64),
            ("Moved", [0x85000000], "3" * 64),
            ("Rematched", [0x84000000], "4" * 64),
            ("Added", [0x86000000], None),
        ])
        changes = symbols.compare_symbol_states(old, new)
        self.assertEqual(changes.added, ("Added",))
        self.assertEqual(changes.removed, ("Removed",))
        self.assertEqual(changes.rebased, ("Moved",))
        self.assertEqual(changes.symbol_changes, ("Rematched",))
        self.assertFalse(changes.build_changed)

    def test_new_build_and_initial_baseline_are_distinct(self):
        current = state([("main", [0x81000000], "2" * 64)])
        baseline = symbols.compare_symbol_states(None, current)
        self.assertEqual(baseline.added, ("main",))
        self.assertFalse(baseline.build_changed)
        changed = state(
            [("main", [0x81000000], "2" * 64)], vpk="9" * 64
        )
        self.assertTrue(symbols.compare_symbol_states(current, changed).build_changed)

    def test_state_loader_rejects_tampering_and_unknown_legacy_format(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "view.gdb"
            state_path = root / "view.state.json"
            script.write_bytes(b"old script\n")
            valid = state([("main", [0x81000000], "2" * 64)])
            state_path.write_text(json.dumps(valid), encoding="utf-8")
            self.assertEqual(
                symbols.load_symbol_state(state_path, script), valid
            )
            script.write_bytes(b"tampered\n")
            with self.assertRaisesRegex(symbols.SymbolError, "does not match"):
                symbols.load_symbol_state(state_path, script)
            script.write_bytes(b"old script\n")
            valid["format"] = "VITADEBUGGER-SYMBOL-STATE-0"
            state_path.write_text(json.dumps(valid), encoding="utf-8")
            with self.assertRaisesRegex(symbols.SymbolError, "unsupported"):
                symbols.load_symbol_state(state_path, script)
            state_path.write_bytes(b'{"format":"x","format":"y"}')
            with self.assertRaisesRegex(symbols.SymbolError, "duplicate JSON"):
                symbols.load_symbol_state(state_path, script)

    def test_publication_rolls_back_if_second_commit_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script_path = root / "view.gdb"
            state_path = root / "view.state.json"
            old_script = "old script\n"
            old_state = state([("main", [0x81000000], "2" * 64)], script=old_script)
            script_path.write_bytes(old_script.encode("utf-8"))
            state_path.write_bytes(
                (json.dumps(old_state, indent=2, sort_keys=True) + "\n").encode("utf-8")
            )
            new_script = "new script\n"
            new_state = state(
                [
                    ("main", [0x81000000], "2" * 64),
                    ("NewModule", [0x82000000], None),
                ],
                script=new_script,
            )
            real_replace = os.replace
            failed = False

            def fail_state_once(source, destination):
                nonlocal failed
                if Path(destination).resolve() == state_path.resolve() and not failed:
                    failed = True
                    raise OSError("injected state commit failure")
                return real_replace(source, destination)

            with patch.object(symbols.os, "replace", side_effect=fail_state_once):
                with self.assertRaisesRegex(symbols.SymbolError, "failed safely"):
                    symbols.publish_symbol_view(
                        script_path, new_script, state_path, new_state
                    )
            self.assertEqual(script_path.read_text(encoding="utf-8"), old_script)
            self.assertEqual(
                json.loads(state_path.read_text(encoding="utf-8")), old_state
            )
            self.assertFalse(list(root.glob("*.part")))

    def test_unchanged_publication_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script_path = root / "view.gdb"
            state_path = root / "view.state.json"
            script = "stable\n"
            current = state([("main", [0x81000000], "2" * 64)], script=script)
            script_path.write_bytes(script.encode("utf-8"))
            state_path.write_bytes(
                (json.dumps(current, indent=2, sort_keys=True) + "\n").encode("utf-8")
            )
            before = (script_path.stat().st_mtime_ns, state_path.stat().st_mtime_ns)
            self.assertFalse(
                symbols.publish_symbol_view(script_path, script, state_path, current)
            )
            self.assertEqual(
                before,
                (script_path.stat().st_mtime_ns, state_path.stat().st_mtime_ns),
            )

    def test_mi_launch_uses_an_argument_vector_and_rejects_script_overrides(self):
        command = symbols.build_gdb_command(
            Path("arm-vita-eabi-gdb.exe"),
            Path("view.gdb"),
            ("--interpreter=mi2", "--quiet"),
            batch=True,
        )
        self.assertEqual(command[1], "-nx")
        self.assertIn("--interpreter=mi2", command)
        self.assertIn("--batch", command)
        self.assertEqual(command[-2], "-x")
        self.assertTrue(command[-1].endswith("view.gdb"))
        for value in ("-ex", "shell rm", "--command=other.gdb", "bad\narg"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(symbols.SymbolError, "unsupported"):
                    symbols.build_gdb_command(
                        Path("gdb"), Path("view.gdb"), (value,), batch=False
                    )

    def test_one_command_refreshes_then_launches_gdb_mi_without_a_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "main.elf"
            gdb = root / "arm-vita-eabi-gdb.exe"
            output = root / "live.gdb"
            make_elf(main, "main")
            gdb.write_bytes(b"test executable placeholder")
            xml = (
                b'<library-list version="1.0"><library name="main">'
                b'<segment address="0x82000000"/><segment address="0x82010000"/>'
                b'</library></library-list>'
            )
            server = MockServer(xml)
            server.thread.start()
            args = SimpleNamespace(
                main_elf=main,
                host="127.0.0.1",
                port=server.port,
                timeout=5,
                search_root=[root],
                module=[],
                main_module=None,
                require_module=[],
                allow_stem_match=False,
                mode="solib",
                solib_cache_root=None,
                build_identity=None,
                allow_unverified_build=True,
                vpk=None,
                verify_installed=False,
                ftp_port=1337,
                ftp_timeout=5,
                output=output,
                state_file=None,
                gdb=gdb,
                gdb_arg=["--interpreter=mi2", "--quiet"],
                gdb_batch=False,
            )
            completed = SimpleNamespace(returncode=7)
            with patch.object(symbols.subprocess, "run", return_value=completed) as launch:
                self.assertEqual(symbols.run(args), 7)
            server.thread.join(2)
            if server.error:
                raise server.error
            self.assertTrue(output.is_file())
            self.assertTrue((root / "live.gdb.state.json").is_file())
            command = launch.call_args.args[0]
            self.assertIn("--interpreter=mi2", command)
            self.assertEqual(command[-2:], ["-x", str(output.resolve())])
            self.assertEqual(launch.call_args.kwargs["shell"], False)


if __name__ == "__main__":
    unittest.main()
