import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
TOOL = TOOLS / "gdb_aslr_lifecycle.py"
SPEC = importlib.util.spec_from_file_location("gdb_aslr_lifecycle", TOOL)
assert SPEC and SPEC.loader
GATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


def image(
    path: Path,
    *,
    link_base: int,
    module_name: str | None,
) -> object:
    segment = GATE.symbols.LoadSegment(link_base, 0x1000, 0x800, 5)
    sections = (
        GATE.symbols.ElfSection(
            ".text", link_base + 0x20, 0x100, 6, 1, 0
        ),
        GATE.symbols.ElfSection(
            ".data", link_base + 0x200, 0x40, 3, 1, 0
        ),
    )
    return GATE.symbols.ElfImage(
        path.resolve(), 2, module_name, (segment,), sections, "a" * 64
    )


def context(
    root: Path,
    *,
    main_base: int = 0x81000000,
    suprx_base: int = 0x82000000,
) -> tuple[object, object, object]:
    main_image = image(
        root / "test.elf", link_base=0x81000000, module_name=None
    )
    suprx_image = image(
        root / "uvdb_aslr_fixture.elf",
        link_base=0x81000000,
        module_name=GATE.DEFAULT_MODULE_NAME,
    )
    main_runtime = GATE.symbols.RuntimeModule("test.elf", (main_base,))
    suprx_runtime = GATE.symbols.RuntimeModule(
        GATE.DEFAULT_MODULE_NAME, (suprx_base,)
    )
    snapshot = GATE.symbols.TargetSnapshot(
        GATE.symbols.OffsetReply("segments", main_base, None),
        (main_runtime, suprx_runtime),
    )
    match = GATE.symbols.ModuleMatch(
        suprx_runtime, suprx_image, "embedded Vita module name"
    )
    resolved = GATE.SymbolContext(
        snapshot, main_runtime, match, (match,), ()
    )
    return main_image, suprx_image, resolved


VALID_TRANSCRIPT = r"""
Breakpoint 1 at 0x81000020: file test.c, line 42.
Breakpoint 2 at 0x82000020: file tests/aslr_fixture/fixture.c, line 19.
UVDB_ASLR_ADDRESSES main=0x81000021 suprx=0x82000021
Thread 1 hit Breakpoint 1, uvdb_aslr_main_breakpoint (sequence=285212673) at D:\build\test.c:42
UVDB_ASLR_MAIN_HIT sequence=0x11000001 pc=0x81000024
Thread 4 hit Breakpoint 2, uvdb_aslr_suprx_breakpoint (sequence=570425345) at D:\build\tests\aslr_fixture\fixture.c:19
UVDB_ASLR_SUPRX_HIT sequence=0x22000001 pc=0x82000024
"""


class GdbAslrLifecycleTests(unittest.TestCase):
    def test_phase_script_arms_main_then_suprx_and_detaches(self):
        script = GATE.render_phase_script(0x11000001, 0x22000001)
        self.assertIn("break uvdb_aslr_main_breakpoint", script)
        self.assertIn("break uvdb_aslr_suprx_breakpoint", script)
        self.assertLess(
            script.index("set variable uvdb_aslr_main_request"),
            script.index("set variable uvdb_aslr_fixture_control.request"),
        )
        self.assertTrue(script.endswith("\n"))
        self.assertIn("detach\nquit\n", script)
        with self.assertRaises(GATE.GateFailure):
            GATE.render_phase_script(0, 1)

    def test_strict_markers_require_both_source_breakpoints(self):
        markers = GATE.parse_gdb_markers(
            VALID_TRANSCRIPT, 0x11000001, 0x22000001
        )
        self.assertEqual(markers.main_address, 0x81000021)
        self.assertEqual(markers.suprx_address, 0x82000021)
        self.assertEqual(markers.main_source, "test.c")
        self.assertEqual(markers.suprx_source, "fixture.c")
        self.assertEqual(markers.main_line, 42)
        self.assertEqual(markers.suprx_line, 19)

    def test_markers_fail_closed_on_wrong_sequence_or_duplicate(self):
        with self.assertRaisesRegex(GATE.GateFailure, "main breakpoint sequence"):
            GATE.parse_gdb_markers(
                VALID_TRANSCRIPT, 0x11000002, 0x22000001
            )
        with self.assertRaisesRegex(GATE.GateFailure, "address marker"):
            GATE.parse_gdb_markers(
                VALID_TRANSCRIPT +
                "UVDB_ASLR_ADDRESSES main=0x1 suprx=0x2\n",
                0x11000001,
                0x22000001,
            )
        with self.assertRaisesRegex(GATE.GateFailure, "SUPRX hit marker"):
            GATE.parse_gdb_markers(
                VALID_TRANSCRIPT.replace("UVDB_ASLR_SUPRX_HIT", "WRONG"),
                0x11000001,
                0x22000001,
            )

    def test_marker_addresses_must_land_in_corresponding_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main_image, suprx_image, resolved = context(root)
            markers = GATE.parse_gdb_markers(
                VALID_TRANSCRIPT, 0x11000001, 0x22000001
            )
            GATE.validate_marker_ranges(
                markers, main_image, suprx_image, resolved
            )
            bad = GATE.GdbMarkers(
                markers.main_address,
                0x81000021,
                markers.main_pc,
                markers.suprx_pc,
                markers.main_sequence,
                markers.suprx_sequence,
                markers.main_source,
                markers.main_line,
                markers.suprx_source,
                markers.suprx_line,
            )
            with self.assertRaisesRegex(GATE.GateFailure, "SUPRX function"):
                GATE.validate_marker_ranges(
                    bad, main_image, suprx_image, resolved
                )

    def test_context_requires_exact_embedded_module_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main_image, suprx_image, resolved = context(root)
            actual = GATE.resolve_context(
                resolved.snapshot,
                main_image,
                suprx_image,
                (suprx_image,),
                GATE.DEFAULT_MODULE_NAME,
            )
            self.assertEqual(actual.suprx_match.image.path, suprx_image.path)
            wrong_name = image(
                root / "uvdb_aslr_fixture.elf",
                link_base=0x81000000,
                module_name="WrongModule",
            )
            with self.assertRaisesRegex(GATE.GateFailure, "exactly one"):
                GATE.resolve_context(
                    resolved.snapshot,
                    main_image,
                    wrong_name,
                    (wrong_name,),
                    GATE.DEFAULT_MODULE_NAME,
                )

    def test_same_process_layout_and_relaunch_delta_are_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, first = context(root)
            _, _, same = context(root)
            GATE.require_same_process_layout(first, same)
            _, _, moved = context(
                root, main_base=0x83000000, suprx_base=0x84000000
            )
            with self.assertRaisesRegex(GATE.GateFailure, "without a process"):
                GATE.require_same_process_layout(first, moved)
            delta = GATE.address_change_record("relaunch", first, moved)
            self.assertTrue(delta["observed"])
            self.assertFalse(
                GATE.address_change_record("repeat", first, same)["observed"]
            )

    def test_public_records_hash_but_do_not_copy_local_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private-name.elf"
            path.write_bytes(b"fixture")
            artifact = GATE.artifact_record(path)
            self.assertEqual(set(artifact), {"sha256", "size_bytes"})
            markers = GATE.parse_gdb_markers(
                VALID_TRANSCRIPT, 0x11000001, 0x22000001
            )
            session = GATE.session_record(
                "phase", "solib", markers, VALID_TRANSCRIPT, 0
            )
            self.assertNotIn("private-name", repr(session))
            self.assertNotIn(VALID_TRANSCRIPT, repr(session))
            self.assertEqual(session["transcript_sha256"],
                             GATE.sha256_text(VALID_TRANSCRIPT))

    def test_snapshot_record_omits_remote_module_names(self):
        snapshot = GATE.symbols.TargetSnapshot(
            GATE.symbols.OffsetReply("segments", 0x81000000, None),
            (
                GATE.symbols.RuntimeModule(
                    "PrivateRemoteModule", (0x81000000,)
                ),
            ),
        )
        record = GATE.snapshot_record("initial", snapshot)
        self.assertEqual(record["module_count"], 1)
        self.assertNotIn("PrivateRemoteModule", repr(record))

    def test_companion_version_must_be_a_portable_short_label(self):
        class FakeCompanion(GATE.CompanionClient):
            def __init__(self, reply):
                self.reply = reply

            def command(self, command):
                self.assert_command = command
                return self.reply

        self.assertEqual(
            FakeCompanion("vitacompanion 1.06").version(),
            "vitacompanion 1.06",
        )
        for reply in (r"C:\private\version", "private/module", "x" * 65):
            with self.subTest(reply=reply):
                with self.assertRaises(GATE.GateFailure):
                    FakeCompanion(reply).version()

    def test_evidence_records_portable_hardware_metadata_without_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "test.elf"
            suprx = root / "uvdb_aslr_fixture.elf"
            velf = root / "uvdb_aslr_fixture.velf"
            vpk = root / "private-package" / "uvdb-test.vpk"
            plugin = root / "private-kernel" / "vitadebug.skprx"
            gdb = root / "private-sdk" / "arm-vita-eabi-gdb.exe"
            vpk.parent.mkdir()
            plugin.parent.mkdir()
            gdb.parent.mkdir()
            for path, content in (
                (main, b"main"),
                (suprx, b"suprx"),
                (velf, b"velf"),
                (vpk, b"vpk"),
                (plugin, b"kernel"),
                (gdb, b"gdb"),
            ):
                path.write_bytes(content)
            args = GATE.argparse.Namespace(
                main_elf=main,
                suprx_elf=suprx,
                suprx_velf=velf,
                suprx_self=None,
                vpk=vpk,
                gdb=gdb,
                title_id="SLRS00001",
                module_name=GATE.DEFAULT_MODULE_NAME,
                include_sensitive_transcript=False,
                device_class="vita-tv",
                firmware="3.65",
                kernel_abi="v1.9",
                kernel_plugin=plugin,
            )
            with patch.object(GATE, "gdb_version", return_value="GDB test"):
                evidence = GATE.create_evidence(args)
            hardware = evidence["hardware"]
            self.assertEqual(hardware["device_class"], "vita-tv")
            self.assertEqual(hardware["firmware"], "3.65")
            self.assertEqual(hardware["kernel_abi"], "v1.9")
            self.assertEqual(
                hardware["kernel_plugin"],
                {
                    "sha256": GATE.sha256_file(plugin),
                    "size_bytes": plugin.stat().st_size,
                },
            )
            self.assertNotIn("vpk", hardware)
            self.assertIn("vpk", evidence["artifacts"])
            self.assertNotIn(str(plugin), repr(evidence))
            self.assertNotIn(str(vpk), repr(evidence))

    def test_optional_hardware_metadata_is_backward_compatible(self):
        self.assertIsNone(
            GATE.portable_hardware_metadata(GATE.argparse.Namespace())
        )

    def test_hardware_metadata_and_kernel_artifact_fail_closed(self):
        for value in ("", " padded", "trailing ", "x" * 65, r"C:\private"):
            with self.subTest(value=value):
                with self.assertRaises(GATE.GateFailure):
                    GATE.validate_portable_metadata("--firmware", value)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "private-missing.skprx"
            with self.assertRaisesRegex(
                GATE.GateFailure, "existing regular file"
            ) as missing_error:
                GATE.portable_kernel_identity(missing)
            self.assertNotIn(str(missing), str(missing_error.exception))
            with self.assertRaisesRegex(
                GATE.GateFailure, "existing regular file"
            ):
                GATE.portable_kernel_identity(root)

    def test_companion_relaunch_is_exact_and_title_scoped(self):
        class FakeCompanion(GATE.CompanionClient):
            def __init__(self):
                self.commands = []

            def command(self, command):
                self.commands.append(command)
                return {
                    "kill SLRS00001": "Killed.",
                    "launch SLRS00001": "Launched.",
                }[command]

        companion = FakeCompanion()
        with patch.object(GATE.time, "sleep"):
            result = companion.relaunch("SLRS00001", 2.5)
        self.assertEqual(
            companion.commands,
            ["kill SLRS00001", "launch SLRS00001"],
        )
        self.assertEqual(result, {"kill": "Killed.", "launch": "Launched."})
        with self.assertRaisesRegex(GATE.GateFailure, "title ID"):
            companion.relaunch("bad", 0)


if __name__ == "__main__":
    unittest.main()
