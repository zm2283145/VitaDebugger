import ftplib
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from contextlib import redirect_stdout


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import gdb_build_identity as generator  # noqa: E402
import gdb_symbols as symbols  # noqa: E402
import uvdb_build_identity as identity  # noqa: E402
from tests.host.test_gdb_symbols import MockServer, make_elf  # noqa: E402


def make_sfo(title_id: str) -> bytes:
    key = b"TITLE_ID\0"
    value = title_id.encode("ascii") + b"\0"
    index_end = 20 + 16
    key_offset = index_end
    data_offset = key_offset + len(key)
    return (
        struct.pack("<4s4I", b"\x00PSF", 0x00000101, key_offset, data_offset, 1)
        + struct.pack("<HHIII", 0, 0x0204, len(value), len(value), 0)
        + key
        + value
    )


def make_vpk(path: Path, *, eboot: bytes = b"main-self", module: bytes = b"module-self") -> Path:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("eboot.bin", eboot)
        archive.writestr("sce_sys/param.sfo", make_sfo("SLRS00001"))
        archive.writestr("module/fixture.suprx", module)
    return path


def create_fixture(root: Path) -> tuple[Path, Path, Path, identity.BuildIdentity]:
    main = root / "main.elf"
    module = root / "fixture.elf"
    vpk = root / "test.vpk"
    make_elf(main, "main")
    make_elf(module, "RuntimeFixture")
    make_vpk(vpk)
    receipt = generator.create_build_identity(
        vpk,
        main,
        {"RuntimeFixture": module},
        {"RuntimeFixture": "module/fixture.suprx"},
        expected_title_id="SLRS00001",
    )
    return main, module, vpk, receipt


class FakeFtp:
    def __init__(self, files: dict[str, bytes], *, connect_error: Exception | None = None):
        self.files = files
        self.connect_error = connect_error
        self.commands: list[object] = []

    def connect(self, host, port, timeout):
        self.commands.append(("connect", host, port, timeout))
        if self.connect_error is not None:
            raise self.connect_error

    def login(self):
        self.commands.append("login")

    def voidcmd(self, command):
        self.commands.append(command)

    def size(self, path):
        self.commands.append(("size", path))
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        return len(self.files[path])

    def retrbinary(self, command, callback, blocksize):
        self.commands.append(("retr", command, blocksize))
        path = command.removeprefix("RETR ")
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        data = self.files[path]
        for offset in range(0, len(data), 3):
            callback(data[offset:offset + 3])

    def quit(self):
        self.commands.append("quit")

    def close(self):
        self.commands.append("close")


class BuildIdentityTests(unittest.TestCase):
    def test_generator_round_trip_binds_vpk_main_and_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main, module, vpk, receipt = create_fixture(root)
            target = generator.write_build_identity(receipt, root / "identity.json")
            loaded = identity.load_build_identity(target)
            self.assertEqual(loaded, receipt)
            self.assertEqual(loaded.title_id, "SLRS00001")
            self.assertEqual(loaded.main_installed.path, "eboot.bin")
            self.assertEqual(loaded.modules[0].installed.path, "module/fixture.suprx")
            identity.verify_local_identity(
                loaded, vpk, main, {"RuntimeFixture": module}
            )

    def test_local_mismatches_and_incomplete_module_sets_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main, module, vpk, receipt = create_fixture(root)
            main.write_bytes(main.read_bytes() + b"changed")
            with self.assertRaisesRegex(identity.BuildIdentityError, "main ELF"):
                identity.verify_local_identity(
                    receipt, vpk, main, {"RuntimeFixture": module}
                )
            make_elf(main, "main")
            vpk.write_bytes(vpk.read_bytes() + b"changed")
            with self.assertRaisesRegex(identity.BuildIdentityError, "VPK"):
                identity.verify_local_identity(
                    receipt, vpk, main, {"RuntimeFixture": module}
                )
            make_vpk(vpk)
            # A packaged module may not be loaded yet. A later refresh will
            # validate it as soon as it appears in the runtime inventory.
            identity.verify_local_identity(receipt, vpk, main, {})
            with self.assertRaisesRegex(identity.BuildIdentityError, "unrecorded"):
                identity.verify_local_identity(
                    receipt,
                    vpk,
                    main,
                    {"RuntimeFixture": module, "Unexpected": module},
                )

    def test_missing_malformed_and_legacy_identity_never_match(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(identity.BuildIdentityError):
                identity.load_build_identity(root / "missing.json")
            with self.assertRaises(identity.BuildIdentityError):
                identity.parse_build_identity(b"not json")
            with self.assertRaisesRegex(identity.BuildIdentityError, "duplicate JSON"):
                identity.parse_build_identity(
                    '{"format":"x","format":"y"}'
                )
            _, _, _, receipt = create_fixture(root)
            legacy = json.loads(identity.identity_to_json(receipt))
            legacy["format"] = 1
            with self.assertRaisesRegex(identity.BuildIdentityError, "unsupported"):
                identity.parse_build_identity(json.dumps(legacy))
            extra = json.loads(identity.identity_to_json(receipt))
            extra["legacy_fallback"] = True
            with self.assertRaisesRegex(identity.BuildIdentityError, "unexpected fields"):
                identity.parse_build_identity(json.dumps(extra))

    def test_generator_rejects_unpaired_or_wrong_module_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "main.elf"
            module = root / "module.elf"
            vpk = make_vpk(root / "test.vpk")
            make_elf(main, "main")
            make_elf(module, "OtherName")
            with self.assertRaisesRegex(identity.BuildIdentityError, "same modules"):
                generator.create_build_identity(
                    vpk, main, {"RuntimeFixture": module}, {}
                )
            with self.assertRaisesRegex(identity.BuildIdentityError, "embeds"):
                generator.create_build_identity(
                    vpk,
                    main,
                    {"RuntimeFixture": module},
                    {"RuntimeFixture": "module/fixture.suprx"},
                )

    def test_installed_verifier_hashes_every_recorded_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, receipt = create_fixture(Path(directory))
            files = {
                "ux0:/app/SLRS00001/eboot.bin": b"main-self",
                "ux0:/app/SLRS00001/module/fixture.suprx": b"module-self",
            }
            fake = FakeFtp(files)
            records = identity.verify_installed_identity(
                receipt,
                "192.0.2.10",
                port=1337,
                timeout=2,
                ftp_factory=lambda: fake,
            )
            self.assertEqual(len(records), 2)
            self.assertIn(
                ("size", "ux0:/app/SLRS00001/eboot.bin"), fake.commands
            )
            self.assertEqual(fake.commands[-1], "quit")

    def test_installed_mismatch_missing_file_and_network_error_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, receipt = create_fixture(Path(directory))
            correct_module = "ux0:/app/SLRS00001/module/fixture.suprx"
            for fake, pattern in (
                (
                    FakeFtp({
                        "ux0:/app/SLRS00001/eboot.bin": b"wrong-self",
                        correct_module: b"module-self",
                    }),
                    "does not match|size",
                ),
                (
                    FakeFtp({"ux0:/app/SLRS00001/eboot.bin": b"main-self"}),
                    "cannot read installed artifact size",
                ),
                (FakeFtp({}, connect_error=OSError("offline")), "over FTP"),
            ):
                with self.subTest(pattern=pattern):
                    with self.assertRaisesRegex(identity.BuildIdentityError, pattern):
                        identity.verify_installed_identity(
                            receipt,
                            "192.0.2.10",
                            ftp_factory=lambda fake=fake: fake,
                        )

    def test_symbol_workflow_enforces_receipt_before_emitting_script(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main, module, vpk, receipt = create_fixture(root)
            receipt_path = generator.write_build_identity(
                receipt, root / "identity.json"
            )
            output = root / "live.gdb"
            xml = (
                b'<library-list version="1.0"><library name="main">'
                b'<segment address="0x82000000"/><segment address="0x82010000"/>'
                b'</library><library name="RuntimeFixture">'
                b'<segment address="0x85000000"/><segment address="0x86000000"/>'
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
                require_module=["RuntimeFixture"],
                allow_stem_match=False,
                mode="solib",
                solib_cache_root=None,
                build_identity=receipt_path,
                allow_unverified_build=False,
                vpk=vpk,
                verify_installed=False,
                ftp_port=1337,
                ftp_timeout=5,
                output=output,
                gdb=None,
            )
            self.assertEqual(symbols.run(args), 0)
            server.thread.join(2)
            if server.error:
                raise server.error
            script = output.read_text(encoding="utf-8")
            self.assertIn(identity.IDENTITY_FORMAT, script)
            self.assertIn("verification=local-only", script)
            self.assertIn(receipt.vpk.sha256, script)
            state_path = root / "live.gdb.state.json"
            before = (output.read_bytes(), state_path.read_bytes())
            main.write_bytes(main.read_bytes() + b"mismatched rebuild")
            with self.assertRaisesRegex(symbols.SymbolError, "main ELF"):
                symbols.run(args)
            self.assertEqual(
                (output.read_bytes(), state_path.read_bytes()), before
            )

    def test_unverified_compatibility_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "main.elf"
            make_elf(main, "main")
            base = dict(
                main_elf=main,
                host="127.0.0.1",
                port=1,
                timeout=1,
                search_root=[root],
                module=[],
                main_module=None,
                require_module=[],
                allow_stem_match=False,
                mode="explicit",
                solib_cache_root=None,
                vpk=None,
                verify_installed=False,
                ftp_port=1337,
                ftp_timeout=1,
                output=root / "out.gdb",
                gdb=None,
            )
            with self.assertRaisesRegex(symbols.SymbolError, "select exactly one"):
                symbols.run(SimpleNamespace(
                    **base,
                    build_identity=None,
                    allow_unverified_build=False,
                ))
            with self.assertRaisesRegex(symbols.SymbolError, "cannot attest"):
                symbols.run(SimpleNamespace(
                    **{**base, "verify_installed": True},
                    build_identity=None,
                    allow_unverified_build=True,
                ))

    def test_identity_backed_refresh_accepts_module_load_then_unload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main, module, vpk, receipt = create_fixture(root)
            receipt_path = generator.write_build_identity(
                receipt, root / "identity.json"
            )
            output = root / "live.gdb"
            state_path = root / "live.state.json"
            main_xml = (
                b'<library-list version="1.0"><library name="main">'
                b'<segment address="0x82000000"/><segment address="0x82010000"/>'
                b'</library></library-list>'
            )
            loaded_xml = main_xml.replace(
                b"</library-list>",
                b'<library name="RuntimeFixture">'
                b'<segment address="0x85000000"/><segment address="0x86000000"/>'
                b'</library></library-list>',
            )

            def arguments(port):
                return SimpleNamespace(
                    main_elf=main,
                    host="127.0.0.1",
                    port=port,
                    timeout=5,
                    search_root=[root],
                    module=[f"RuntimeFixture={module}"],
                    main_module=None,
                    require_module=[],
                    allow_stem_match=False,
                    mode="solib",
                    solib_cache_root=None,
                    build_identity=receipt_path,
                    allow_unverified_build=False,
                    vpk=vpk,
                    verify_installed=False,
                    ftp_port=1337,
                    ftp_timeout=5,
                    output=output,
                    state_file=state_path,
                    gdb=None,
                    gdb_arg=[],
                    gdb_batch=False,
                )

            transcripts = []
            for xml in (main_xml, loaded_xml, main_xml):
                server = MockServer(xml)
                server.thread.start()
                capture = io.StringIO()
                with redirect_stdout(capture):
                    self.assertEqual(symbols.run(arguments(server.port)), 0)
                server.thread.join(2)
                if server.error:
                    raise server.error
                transcripts.append(capture.getvalue())
            self.assertIn("added=RuntimeFixture", transcripts[1])
            self.assertIn("removed=RuntimeFixture", transcripts[2])
            final = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual([item["name"] for item in final["modules"]], ["main"])


if __name__ == "__main__":
    unittest.main()
