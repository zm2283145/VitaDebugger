import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
SPEC = importlib.util.spec_from_file_location(
    "gdb_exception_memory_fileio_gate",
    TOOLS / "gdb_exception_memory_fileio_gate.py",
)
gate = importlib.util.module_from_spec(SPEC)
with mock.patch.dict("sys.modules", {"gdb_symbols": mock.MagicMock()}):
    sys.modules[SPEC.name] = gate
    SPEC.loader.exec_module(gate)


class Segment:
    def __init__(self, vaddr, memsz):
        self.vaddr = vaddr
        self.memsz = memsz


class Image:
    segments = (Segment(0x1000, 0x200), Segment(0x3000, 0x100))


class Runtime:
    segments = (0x81000000, 0x82000000)


class GateTests(unittest.TestCase):
    def test_relocates_each_symbol_through_owning_segment(self):
        result = gate.relocate_symbols(
            Image(), Runtime(), {"code": 0x1010, "data": 0x3040}
        )
        self.assertEqual(
            result, {"code": 0x81000010, "data": 0x82000040}
        )

    def test_rejects_symbol_outside_load_segments(self):
        with self.assertRaises(gate.GateFailure):
            gate.relocate_symbols(Image(), Runtime(), {"bad": 0x5000})

    def test_candidate_requires_exact_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "candidate.elf"
            vpk = Path(directory) / "candidate.vpk"
            elf.write_bytes(b"elf")
            vpk.write_bytes(b"vpk")
            candidate = gate.require_candidate(
                elf,
                vpk,
                gate.sha256_file(elf),
                gate.sha256_file(vpk),
            )
            self.assertEqual(candidate.elf_sha256, gate.sha256_file(elf))
            with self.assertRaises(gate.GateFailure):
                gate.require_candidate(elf, vpk, "0" * 64, gate.sha256_file(vpk))

    def test_memory_pattern_is_three_chunks(self):
        self.assertEqual(gate.MEMORY_SIZE, 3 * 64)


if __name__ == "__main__":
    unittest.main()
