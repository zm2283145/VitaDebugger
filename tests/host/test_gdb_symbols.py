import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import gdb_symbols as symbols  # noqa: E402


def make_elf(
    path: Path,
    module_name: str = "sample.elf",
    stripped: bool = False,
    elf_type: int = 2,
    include_module_info: bool = True,
    symbol_name: str = "probe_symbol",
):
    names = [
        "", ".text", ".data", ".bss", ".sceModuleInfo.rodata",
        ".symtab", ".strtab", ".shstrtab",
    ]
    shstr = bytearray(b"\0")
    name_offsets = {"": 0}
    for name in names[1:]:
        name_offsets[name] = len(shstr)
        shstr.extend(name.encode("ascii") + b"\0")

    data = bytearray(0x800)
    phoff = 52
    shoff = 0x500
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\0" * 9,
        elf_type, 40, 1, 0x81000020, phoff, shoff, 0,
        52, 32, 2, 40, 8, 7,
    )
    data[:52] = header
    struct.pack_into(
        "<IIIIIIII", data, phoff,
        1, 0x100, 0x81000000, 0, 0x100, 0x100, 5, 0x1000,
    )
    struct.pack_into(
        "<IIIIIIII", data, phoff + 32,
        1, 0x200, 0x81010000, 0, 0x10, 0x40, 6, 0x1000,
    )
    data[0x120:0x140] = b"\x11" * 0x20
    data[0x180:0x184] = b"\0\0\x01\x01"
    if include_module_info:
        encoded_name = module_name.encode("ascii")
        data[0x184:0x184 + len(encoded_name)] = encoded_name
        data[0x184 + len(encoded_name)] = 0
    data[0x200:0x210] = b"\x22" * 0x10
    data[0x300:0x300 + len(shstr)] = shstr
    string_table = b"\0" + symbol_name.encode("ascii") + b"\0"
    data[0x400:0x410] = b"\0" * 16
    struct.pack_into(
        "<IIIBBH", data, 0x410,
        1, 0x81000024, 4, 0x12, 0, 1,
    )
    data[0x420:0x420 + len(string_table)] = string_table

    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (name_offsets[".text"], 1, 6, 0x81000020, 0x120, 0x20, 0, 0, 4, 0),
        (name_offsets[".data"], 1, 3, 0x81010000, 0x200, 0x10, 0, 0, 4, 0),
        (name_offsets[".bss"], 8, 3, 0x81010010, 0x210, 0x20, 0, 0, 4, 0),
        (
            name_offsets[".sceModuleInfo.rodata"], 1, 2, 0x81000080,
            0x180, 0x40, 0, 0, 4, 0,
        ),
        (
            name_offsets[".symtab"], 1 if stripped else 2, 0, 0,
            0x400, 0x20, 6, 1, 4, 16,
        ),
        (
            name_offsets[".strtab"], 3, 0, 0, 0x420,
            len(string_table), 0, 0, 1, 0,
        ),
        (
            name_offsets[".shstrtab"], 3, 0, 0, 0x300,
            len(shstr), 0, 0, 1, 0,
        ),
    ]
    for index, section in enumerate(sections):
        struct.pack_into("<IIIIIIIIII", data, shoff + index * 40, *section)
    path.write_bytes(data)


def frame(payload: bytes) -> bytes:
    return b"$" + payload + f"#{sum(payload) & 0xff:02x}".encode("ascii")


class MockServer:
    def __init__(self, xml: bytes):
        self.xml = xml
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.error = None
        self.requests = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    @staticmethod
    def read_packet(connection):
        while connection.recv(1) != b"$":
            pass
        payload = bytearray()
        while True:
            value = connection.recv(1)
            if value == b"#":
                break
            if not value:
                raise RuntimeError("client disconnected")
            payload.extend(value)
        connection.recv(2)
        return bytes(payload)

    def reply(self, request):
        if request.startswith(b"qSupported"):
            return b"qXfer:libraries:read+;PacketSize=120"
        if request == b"QStartNoAckMode":
            return b"OK"
        if request == b"?":
            return b"T05thread:1;"
        if request == b"qOffsets":
            return b"TextSeg=82000000;DataSeg=82010000"
        if request == b"g":
            return b"0" * 336
        if request == b"qfThreadInfo":
            return b"m1"
        if request == b"qsThreadInfo":
            return b"l"
        if request == b"qC":
            return b"QC1"
        if request == b"qAttached":
            return b"1"
        if request.startswith((b"H", b"T")):
            return b"OK"
        if request.startswith(b"qThreadExtraInfo"):
            return b"6d6f636b"
        if request.startswith(b"qXfer:libraries:read::"):
            offset_length = request.rsplit(b":", 1)[1]
            offset_text, length_text = offset_length.split(b",")
            offset = int(offset_text, 16)
            length = int(length_text, 16)
            chunk = self.xml[offset:offset + length]
            marker = b"l" if offset + len(chunk) >= len(self.xml) else b"m"
            return marker + chunk
        if request == b"D":
            return b"OK"
        return b""

    def run(self):
        ack_mode = True
        try:
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(5)
                while True:
                    request = self.read_packet(connection)
                    self.requests.append(request)
                    if ack_mode:
                        connection.sendall(b"+")
                    connection.sendall(frame(self.reply(request)))
                    if ack_mode:
                        if connection.recv(1) != b"+":
                            raise RuntimeError("missing client acknowledgement")
                    if request == b"QStartNoAckMode":
                        ack_mode = False
                    if request == b"D":
                        return
        except Exception as exc:
            self.error = exc
        finally:
            self.listener.close()


class ElfTests(unittest.TestCase):
    def test_reads_embedded_name_segments_and_relocates_sections(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "symbols.velf"
            make_elf(path, "RuntimeModule")
            image = symbols.read_elf(path)
            self.assertEqual(image.module_name, "RuntimeModule")
            self.assertEqual([segment.vaddr for segment in image.segments],
                             [0x81000000, 0x81010000])
            addresses = image.loaded_section_addresses([0x83000000, 0x84000000])
            self.assertEqual(addresses[".text"], 0x83000020)
            self.assertEqual(addresses[".data"], 0x84000000)
            self.assertEqual(addresses[".bss"], 0x84000010)

    def test_rejects_stripped_and_packed_files(self):
        with tempfile.TemporaryDirectory() as directory:
            stripped = Path(directory) / "stripped.elf"
            make_elf(stripped, stripped=True)
            with self.assertRaisesRegex(symbols.ElfError, "symtab"):
                symbols.read_elf(stripped)
            packed = Path(directory) / "module.suprx"
            packed.write_bytes(b"SCE\0" + b"\0" * 64)
            with self.assertRaisesRegex(symbols.ElfError, "SELF/SUPRX"):
                symbols.read_elf(packed)

    def test_vita_converted_file_is_only_a_verified_name_sidecar(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            linked = root / "Plugin.elf"
            converted = root / "Plugin.velf"
            make_elf(linked, include_module_info=False)
            make_elf(converted, "RuntimePlugin", elf_type=0xFE04)
            with self.assertRaisesRegex(symbols.ElfError, "original linked"):
                symbols.read_elf(converted)
            scanned = symbols.scan_elfs((root,))
            self.assertEqual(len(scanned), 1)
            self.assertEqual(scanned[0].path, linked.resolve())
            self.assertEqual(scanned[0].module_name, "RuntimePlugin")


class ProtocolTests(unittest.TestCase):
    def test_offsets_reconcile_every_main_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "main.elf"
            make_elf(path, "main")
            image = symbols.read_elf(path)
            offsets = symbols.parse_qoffsets(
                "TextSeg=82000000;DataSeg=83000000"
            )
            main = symbols.RuntimeModule("main", (0x82000000, 0x83000000))
            snapshot = symbols.TargetSnapshot(offsets, (main,))
            self.assertIs(symbols.reconcile_main(image, snapshot), main)
            wrong = symbols.RuntimeModule("main", (0x82000000, 0x83001000))
            with self.assertRaisesRegex(symbols.SymbolError, "exactly one"):
                symbols.reconcile_main(
                    image, symbols.TargetSnapshot(offsets, (wrong,))
                )

    def test_zero_relocation_offsets_are_valid_but_zero_segment_bases_are_not(self):
        reply = symbols.parse_qoffsets("Text=0;Data=0;Bss=0")
        self.assertEqual(reply, symbols.OffsetReply("offsets", 0, 0))
        with self.assertRaises(symbols.ProtocolError):
            symbols.parse_qoffsets("TextSeg=0;DataSeg=82010000")

    def test_library_xml_is_strict_and_bounded(self):
        parsed = symbols.parse_library_xml(
            b'<library-list version="1.0"><library name="main">'
            b'<segment address="0x82000000"/></library></library-list>'
        )
        self.assertEqual(parsed[0].segments, (0x82000000,))
        with self.assertRaises(symbols.ProtocolError):
            symbols.parse_library_xml(
                b'<!DOCTYPE x><library-list version="1.0"></library-list>'
            )
        with self.assertRaises(symbols.ProtocolError):
            symbols.parse_library_xml(
                b'<library-list version="1.0"><library name="bad"/>'
                b'</library-list>'
            )

    def test_live_query_chunks_and_detaches(self):
        filler = b"".join(
            f'<library name="lib{index:02d}"><segment address="0x{0x90000000 + index * 0x1000:08x}"/></library>'.encode()
            for index in range(8)
        )
        xml = (
            b'<library-list version="1.0"><library name="main">'
            b'<segment address="0x82000000"/><segment address="0x82010000"/>'
            b'</library>' + filler + b'</library-list>'
        )
        server = MockServer(xml)
        server.thread.start()
        query_error = None
        try:
            snapshot = symbols.query_target("127.0.0.1", server.port, 5)
        except Exception as exc:
            query_error = exc
        finally:
            server.thread.join(2)
        if server.error:
            raise server.error
        if query_error:
            raise query_error
        self.assertEqual(snapshot.offsets.text, 0x82000000)
        self.assertEqual(len(snapshot.modules), 9)
        self.assertGreater(
            len([request for request in server.requests
                 if request.startswith(b"qXfer:libraries:read::")]),
            1,
        )
        self.assertEqual(server.requests[-1], b"D")


class MatchingTests(unittest.TestCase):
    def test_unique_match_and_generated_section_addresses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main_path = root / "main.elf"
            lib_path = root / "libsymbols.elf"
            make_elf(main_path, "main")
            make_elf(lib_path, "RuntimeLib")
            main_image = symbols.read_elf(main_path)
            lib_image = symbols.read_elf(lib_path)
            main = symbols.RuntimeModule("main", (0x82000000, 0x82010000))
            library = symbols.RuntimeModule(
                "RuntimeLib", (0x85000000, 0x86000000)
            )
            snapshot = symbols.TargetSnapshot(
                symbols.parse_qoffsets(
                    "TextSeg=82000000;DataSeg=82010000"
                ),
                (main, library),
            )
            matches, unmatched = symbols.match_modules(
                snapshot, main, (main_image, lib_image), {}
            )
            self.assertFalse(unmatched)
            self.assertEqual(matches[0].reason, "embedded Vita module name")
            script = symbols.render_gdb_script(
                "192.0.2.10", 1234, snapshot, main, main_image,
                matches, unmatched,
            )
            self.assertIn("set auto-solib-add off", script)
            self.assertIn("target remote 192.0.2.10:1234", script)
            self.assertIn("0x85000020", script)
            self.assertIn("-s .data 0x86000000", script)
            self.assertIn("regenerate after every target relaunch", script)

    def test_ambiguous_name_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "a.velf"
            second = root / "b.velf"
            make_elf(first, "SameName")
            make_elf(second, "SameName")
            main = symbols.RuntimeModule("main", (1, 2))
            library = symbols.RuntimeModule("SameName", (3, 4))
            snapshot = symbols.TargetSnapshot(
                symbols.OffsetReply("segments", 1, 2), (main, library)
            )
            with self.assertRaisesRegex(symbols.SymbolError, "ambiguous"):
                symbols.match_modules(
                    snapshot, main,
                    (symbols.read_elf(first), symbols.read_elf(second)),
                    {},
                )

    def test_filename_fallback_rejects_contradictory_embedded_name(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "RuntimeLib"
            make_elf(path, "DifferentEmbeddedName")
            image = symbols.read_elf(path)
            main = symbols.RuntimeModule("main", (1, 2))
            library = symbols.RuntimeModule(
                "RuntimeLib", (0x85000000, 0x86000000)
            )
            snapshot = symbols.TargetSnapshot(
                symbols.OffsetReply("segments", 1, 2), (main, library)
            )
            matches, unmatched = symbols.match_modules(
                snapshot, main, (image,), {}, True
            )
            self.assertFalse(matches)
            self.assertEqual(unmatched, (library,))

    def test_typo_in_explicit_mapping_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lib.velf"
            make_elf(path, "LiveName")
            image = symbols.read_elf(path)
            main = symbols.RuntimeModule("main", (1, 2))
            snapshot = symbols.TargetSnapshot(
                symbols.OffsetReply("segments", 1, 2), (main,)
            )
            with self.assertRaisesRegex(symbols.SymbolError, "not present"):
                symbols.match_modules(
                    snapshot, main, (image,), {"TypoName": image}
                )


class WorkflowTests(unittest.TestCase):
    def test_end_to_end_snapshot_scan_and_script_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main_path = root / "main.elf"
            library_path = root / "RuntimeLib.elf"
            output = root / "live.gdb"
            make_elf(main_path, "main")
            make_elf(library_path, "RuntimeLib")
            xml = (
                b'<library-list version="1.0"><library name="main">'
                b'<segment address="0x82000000"/><segment address="0x82010000"/>'
                b'</library><library name="RuntimeLib">'
                b'<segment address="0x85000000"/><segment address="0x86000000"/>'
                b'</library></library-list>'
            )
            server = MockServer(xml)
            server.thread.start()
            arguments = SimpleNamespace(
                main_elf=main_path,
                host="127.0.0.1",
                port=server.port,
                timeout=5,
                search_root=[root],
                module=[],
                main_module=None,
                require_module=["RuntimeLib"],
                allow_stem_match=False,
                mode="solib",
                solib_cache_root=None,
                output=output,
                gdb=None,
            )
            self.assertEqual(symbols.run(arguments), 0)
            server.thread.join(2)
            if server.error:
                raise server.error
            generated = output.read_text(encoding="utf-8")
            self.assertIn("file \"", generated)
            self.assertIn("set auto-solib-add on", generated)
            self.assertIn("sharedlibrary", generated)
            self.assertNotIn("add-symbol-file", generated)
            self.assertIn("RuntimeLib", generated)
            cached = list((root / ".uvdb-solib").glob("*/RuntimeLib"))
            self.assertEqual(len(cached), 1)
            self.assertEqual(
                symbols.sha256_file(cached[0]), symbols.sha256_file(library_path)
            )

    def test_real_gdb_uses_qoffsets_and_solib_segments(self):
        vitasdk = Path(os.environ.get("VITASDK", r"C:\vitasdk"))
        gdb_name = "arm-vita-eabi-gdb.exe" if os.name == "nt" else "arm-vita-eabi-gdb"
        gdb = vitasdk / "bin" / gdb_name
        if not gdb.exists():
            self.skipTest("arm-vita-eabi-gdb is not installed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main_path = root / "main.elf"
            library_path = root / "library.elf"
            make_elf(main_path, "main", symbol_name="main_probe")
            make_elf(library_path, "RuntimeLib", symbol_name="library_probe")
            library_image = symbols.read_elf(library_path)
            runtime_library = symbols.RuntimeModule(
                "RuntimeLib", (0x85000000, 0x86000000)
            )
            match = symbols.ModuleMatch(
                runtime_library, library_image, "test mapping"
            )
            cache = symbols.materialize_solib_cache((match,), root / "cache")
            main_image = symbols.read_elf(main_path)
            main_runtime = symbols.RuntimeModule(
                "main", (0x82000000, 0x82010000)
            )
            snapshot = symbols.TargetSnapshot(
                symbols.parse_qoffsets(
                    "TextSeg=82000000;DataSeg=82010000"
                ),
                (main_runtime, runtime_library),
            )
            xml = (
                b'<library-list version="1.0"><library name="main">'
                b'<segment address="0x82000000"/><segment address="0x82010000"/>'
                b'</library><library name="RuntimeLib">'
                b'<segment address="0x85000000"/><segment address="0x86000000"/>'
                b'</library></library-list>'
            )
            server = MockServer(xml)
            server.thread.start()
            script = symbols.render_gdb_script(
                "127.0.0.1", server.port, snapshot, main_runtime, main_image,
                (match,), (), "solib", cache,
            )
            script += (
                "info address main_probe\n"
                "info address library_probe\n"
                "detach\n"
            )
            script_path = root / "integration.gdb"
            script_path.write_text(script, encoding="utf-8", newline="\n")
            command = [str(gdb), "-nx", "-batch", "-x", str(script_path)]
            try:
                result = subprocess.run(
                    command, capture_output=True, text=True, timeout=15
                )
            except subprocess.TimeoutExpired as exc:
                server.thread.join(1)
                self.fail(
                    f"GDB timed out; requests={server.requests!r}; "
                    f"stdout={exc.stdout!r}; stderr={exc.stderr!r}; "
                    f"server={server.error!r}"
                )
            server.thread.join(2)
            output = result.stdout + result.stderr
            if server.error:
                self.fail(
                    f"mock server failed: {server.error!r}; "
                    f"requests={server.requests!r}; output={output}"
                )
            self.assertEqual(result.returncode, 0, output)
            self.assertIn("0x82000024", output, output)
            self.assertIn("0x85000024", output, output)


if __name__ == "__main__":
    unittest.main()
