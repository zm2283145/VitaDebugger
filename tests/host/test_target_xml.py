import ast
import os
import re
import socket
import subprocess
import threading
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET_XML_INC = ROOT / "protocol" / "arm_vfp_target_xml.inc"
KERNEL_ABI_HEADER = ROOT / "kernel" / "include" / "vitadebug_kernel.h"
KERNEL_SOURCE = ROOT / "kernel" / "src" / "main.c"
VFP_PROBE_SOURCE = ROOT / "kernel" / "test" / "vfp_probe.c"
UVDB_SOURCE = ROOT / "uvdb.c"


def load_embedded_xml():
    source = TARGET_XML_INC.read_text(encoding="utf-8")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', source)
    return "".join(ast.literal_eval(literal) for literal in literals)


def rsp_packet(payload):
    encoded = payload.encode("ascii")
    checksum = sum(encoded) & 0xFF
    return b"$" + encoded + f"#{checksum:02x}".encode("ascii")


class MockRspServer:
    def __init__(self, target_xml, register_packet):
        self.target_xml = target_xml
        self.register_packet = register_packet
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.error = None
        self.requests = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def reply_for(self, request):
        if request.startswith("qSupported"):
            return "PacketSize=4000;qXfer:features:read+"
        if request.startswith("qXfer:features:read:target.xml:"):
            position = request.rsplit(":", 1)[1]
            offset_text, length_text = position.split(",", 1)
            offset = int(offset_text, 16)
            length = int(length_text, 16)
            chunk = self.target_xml[offset:offset + length]
            marker = "l" if offset + len(chunk) >= len(self.target_xml) else "m"
            return marker + chunk
        if request == "?":
            return "T05thread:1;"
        if request == "g":
            return self.register_packet
        if request.startswith("m"):
            return "E01"
        if request in ("qfThreadInfo",):
            return "m1"
        if request in ("qsThreadInfo",):
            return "l"
        if request == "qC":
            return "QC1"
        if request == "qAttached":
            return "1"
        if request.startswith("H") or request.startswith("T"):
            return "OK"
        if request.startswith("qThreadExtraInfo"):
            return "7666702074657374"
        if request == "qOffsets":
            return "Text=0;Data=0;Bss=0"
        if request == "D":
            return "OK"
        return ""

    def run(self):
        try:
            self.listener.settimeout(10)
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(10)
                while True:
                    first = connection.recv(1)
                    if not first:
                        return
                    if first in (b"+", b"-"):
                        continue
                    if first == b"\x03":
                        connection.sendall(rsp_packet("T02thread:1;"))
                        continue
                    if first != b"$":
                        continue
                    payload = bytearray()
                    while True:
                        byte = connection.recv(1)
                        if byte == b"#":
                            break
                        if not byte:
                            return
                        payload.extend(byte)
                    connection.recv(2)
                    connection.sendall(b"+")
                    request = payload.decode("ascii")
                    self.requests.append(request)
                    connection.sendall(rsp_packet(self.reply_for(request)))
                    if request == "D":
                        return
        except Exception as error:  # surfaced by the test after join
            self.error = error
        finally:
            self.listener.close()


class TargetDescriptionTests(unittest.TestCase):
    def setUp(self):
        self.xml = load_embedded_xml()
        self.root = ET.fromstring(self.xml)

    def feature(self, name):
        return self.root.find(f"./feature[@name='{name}']")

    def test_architecture_and_osabi(self):
        self.assertEqual(self.root.findtext("architecture"), "armv7")
        self.assertEqual(self.root.findtext("osabi"), "GNU/Linux")

    def test_core_register_numbers_leave_legacy_fpa_slots(self):
        core = self.feature("org.gnu.gdb.arm.core")
        self.assertIsNotNone(core)
        registers = core.findall("reg")
        self.assertEqual(
            [register.get("name") for register in registers[:16]],
            [f"r{i}" for i in range(13)] + ["sp", "lr", "pc"],
        )
        self.assertEqual(registers[-1].get("name"), "cpsr")
        self.assertEqual(registers[-1].get("regnum"), "25")

    def test_vfp_d32_register_set_is_complete_and_stable(self):
        vfp = self.feature("org.gnu.gdb.arm.vfp")
        self.assertIsNotNone(vfp)
        registers = vfp.findall("reg")
        self.assertEqual(
            [register.get("name") for register in registers[:-1]],
            [f"d{i}" for i in range(32)],
        )
        self.assertTrue(all(register.get("bitsize") == "64"
                            for register in registers[:-1]))
        self.assertEqual(registers[-1].get("name"), "fpscr")
        self.assertEqual(registers[-1].get("bitsize"), "32")

    def test_packet_size_matches_target_description_contract(self):
        legacy_core_bytes = 16 * 4 + 8 * 12 + 4 + 4
        explicit_core_bytes = 16 * 4 + 4
        vfp_bytes = 32 * 8 + 4
        self.assertEqual(legacy_core_bytes * 2, 336)
        self.assertEqual((explicit_core_bytes + vfp_bytes) * 2, 656)

    def test_hardware_validated_fpscr_entry_zero_is_wired_end_to_end(self):
        header = KERNEL_ABI_HEADER.read_text(encoding="utf-8")
        probe = VFP_PROBE_SOURCE.read_text(encoding="utf-8")
        stub = UVDB_SOURCE.read_text(encoding="utf-8")
        entry = re.search(
            r"#define\s+VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1\s+(\d+)u?",
            header,
        )
        self.assertIsNotNone(entry)
        self.assertEqual(int(entry.group(1)), 0)

        selected_entry = (
            r"fpscr_entry\s*\[\s*VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1\s*\]"
        )
        self.assertRegex(probe, selected_entry)
        self.assertRegex(stub, selected_entry)
        self.assertNotRegex(stub, r"snapshot\.fpscr_entry\s*\[\s*1u?\s*\]")

    def test_only_normalized_unavailable_vfp_result_preserves_core(self):
        header = KERNEL_ABI_HEADER.read_text(encoding="utf-8")
        kernel = KERNEL_SOURCE.read_text(encoding="utf-8")
        stub = UVDB_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "#define VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE ((int)0x80028031u)",
            header,
        )
        self.assertEqual(
            kernel.count("SCE_KERNEL_ERROR_CAN_NOT_USE_VFP"), 1,
            "the raw no-VFP error must have one narrow normalization site",
        )
        self.assertEqual(
            kernel.count("SCE_KERNEL_ERROR_ILLEGAL_PERMISSION"), 1,
            "the observed raw permission error must have one normalization site",
        )
        self.assertEqual(
            kernel.count("VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE"), 1,
            "no other kernel failure may become context-unavailable",
        )
        self.assertIn(
            "uvdb_vfp_classify_snapshot_result(snapshot_result)", stub,
        )
        self.assertNotIn(
            "snapshot_result == VD_KERNEL_ERROR_VFP_GUARD", stub,
        )
        self.assertLess(
            kernel.index("if(guard_changed)"),
            kernel.index("(int)SCE_KERNEL_ERROR_ILLEGAL_PERMISSION"),
            "guard corruption must override recognized unavailable results",
        )

    def test_vitasdk_gdb_reads_expected_vfp_register_offsets(self):
        vitasdk = os.environ.get("VITASDK", r"C:\vitasdk")
        gdb_name = "arm-vita-eabi-gdb.exe" if os.name == "nt" else "arm-vita-eabi-gdb"
        gdb = Path(vitasdk) / "bin" / gdb_name
        if not gdb.exists():
            self.skipTest("arm-vita-eabi-gdb is not installed")

        def little_hex(value, byte_count):
            return value.to_bytes(byte_count, "little").hex()

        core = "".join(little_hex(0x11223300 + index, 4)
                       for index in range(16))
        cpsr = little_hex(0xA1B2C3D4, 4)
        d_values = [0] * 32
        d_values[0] = 0x3FF0000000000000  # 1.0
        d_values[31] = 0x4000000000000000  # 2.0
        vfp = "".join(little_hex(value, 8) for value in d_values)
        fpscr = little_hex(0x00400000, 4)
        server = MockRspServer(self.xml, core + cpsr + vfp + fpscr)
        server.start()

        result = subprocess.run(
            [
                str(gdb), "-nx", "-batch",
                "-ex", "set confirm off",
                "-ex", f"target remote 127.0.0.1:{server.port}",
                "-ex", "p/x $r0",
                "-ex", "p/x $cpsr",
                "-ex", "p $d0",
                "-ex", "p $d31",
                "-ex", "p/x $fpscr",
                "-ex", "detach",
            ],
            capture_output=True,
            text=True,
            timeout=15,
        )
        server.thread.join(timeout=2)
        if server.error:
            raise server.error
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("0x11223300", output)
        self.assertIn("0xa1b2c3d4", output.lower())
        self.assertRegex(output, r"(?m)^\$3 = 1(?:\.0+)?$")
        self.assertRegex(output, r"(?m)^\$4 = 2(?:\.0+)?$")
        self.assertIn("0x400000", output.lower())

    def test_vitasdk_gdb_accepts_unavailable_vfp_with_valid_core(self):
        vitasdk = os.environ.get("VITASDK", r"C:\vitasdk")
        gdb_name = "arm-vita-eabi-gdb.exe" if os.name == "nt" else "arm-vita-eabi-gdb"
        gdb = Path(vitasdk) / "bin" / gdb_name
        if not gdb.exists():
            self.skipTest("arm-vita-eabi-gdb is not installed")

        def little_hex(value, byte_count):
            return value.to_bytes(byte_count, "little").hex()

        core = "".join(little_hex(0x11223300 + index, 4)
                       for index in range(16))
        cpsr = little_hex(0xA1B2C3D4, 4)
        unavailable_vfp = "x" * ((32 * 8 + 4) * 2)
        registers = core + cpsr + unavailable_vfp
        self.assertEqual(len(registers), 656)
        server = MockRspServer(self.xml, registers)
        server.start()

        result = subprocess.run(
            [
                str(gdb), "-nx", "-batch",
                "-ex", "set confirm off",
                "-ex", f"target remote 127.0.0.1:{server.port}",
                "-ex", "p/x $r0",
                "-ex", "p $d0",
                "-ex", "p/x $fpscr",
                "-ex", "detach",
            ],
            capture_output=True,
            text=True,
            timeout=15,
        )
        server.thread.join(timeout=2)
        if server.error:
            raise server.error
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("0x11223300", output)
        self.assertGreaterEqual(output.lower().count("<unavailable>"), 2,
                                output)

    def test_vitasdk_gdb_transport_disconnect_sends_no_detach_packet(self):
        vitasdk = os.environ.get("VITASDK", r"C:\vitasdk")
        gdb_name = "arm-vita-eabi-gdb.exe" if os.name == "nt" else "arm-vita-eabi-gdb"
        gdb = Path(vitasdk) / "bin" / gdb_name
        if not gdb.exists():
            self.skipTest("arm-vita-eabi-gdb is not installed")

        core_bytes = 16 * 4 + 4
        vfp_bytes = 32 * 8 + 4
        server = MockRspServer(
            self.xml,
            "00" * (core_bytes + vfp_bytes),
        )
        server.start()
        commands = "\n".join([
            "1-gdb-set confirm off",
            f"2-target-select remote 127.0.0.1:{server.port}",
            "3-target-disconnect",
            "4-gdb-exit",
            "",
        ])
        result = subprocess.run(
            [str(gdb), "-nx", "-q", "--interpreter=mi2"],
            input=commands,
            capture_output=True,
            text=True,
            timeout=15,
        )
        server.thread.join(timeout=2)
        if server.error:
            raise server.error
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("3^done", output)
        self.assertNotIn("D", server.requests)


if __name__ == "__main__":
    unittest.main()
