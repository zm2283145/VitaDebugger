import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import ANY, call, patch


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
TOOL = TOOLS / "gdb_step_register_gate.py"
SPEC = importlib.util.spec_from_file_location("gdb_step_register_gate", TOOL)
assert SPEC and SPEC.loader
GATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


class StepRegisterGateTests(unittest.TestCase):
    def test_generated_symbol_script_selects_one_matching_main_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "main image.elf"
            elf.write_bytes(b"ELF")
            script = root / "symbols.gdb"
            script.write_text(
                'set pagination off\n'
                'file "main image.elf"\n'
                'target remote vita.test:1234\n',
                encoding="utf-8",
            )
            self.assertEqual(
                GATE.main_elf_from_symbol_script(script, "vita.test", 1234),
                elf.resolve(),
            )

    def test_symbol_script_rejects_ambiguous_or_wrong_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "symbols.gdb"
            script.write_text(
                'file "one.elf"\nfile "two.elf"\n'
                'target remote 192.0.2.10:1234\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateFailure, "exactly one"):
                GATE.main_elf_from_symbol_script(script, "192.0.2.10", 1234)
            script.write_text(
                'file "one.elf"\ntarget remote 192.0.2.11:1234\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateFailure, "not requested"):
                GATE.main_elf_from_symbol_script(script, "192.0.2.10", 1234)

    def test_gdb_integer_parser_is_bounded_and_accepts_symbol_suffix(self):
        self.assertEqual(GATE.parse_gdb_integer("0x81001234 <fixture>"), 0x81001234)
        self.assertEqual(GATE.parse_gdb_integer("4294967295"), 0xFFFFFFFF)
        for invalid in ("-1", "0x100000000", "0x1 trailing", "", "(void *) 1"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(GATE.GateFailure):
                    GATE.parse_gdb_integer(invalid)

    def test_current_thread_accepts_decimal_and_hex_target_ids(self):
        class FakeMi:
            token = 1

            def __init__(self, target_id):
                self.target_id = target_id

            def command(self, command):
                self.command_seen = command
                return (
                    '1^done,threads=[{id="1",target-id="'
                    + self.target_id
                    + '",name="test main"}],current-thread-id="1"',
                    [],
                )

        for spelling, expected in (
            ("Thread 1073807363", 1073807363),
            ("Thread 0x40010003", 0x40010003),
            ("1073807363", 1073807363),
        ):
            with self.subTest(spelling=spelling):
                selected = GATE.current_thread(FakeMi(spelling))
                self.assertEqual(selected.target_id, expected)

        with self.assertRaisesRegex(GATE.GateFailure, "unsupported"):
            GATE.current_thread(FakeMi("Thread -1"))

    def test_monitor_breakpoint_count_requires_one_exact_decimal_field(self):
        report = "VitaDebugger status\n  software-breakpoints: 2\n"
        self.assertEqual(GATE.breakpoint_count(report), 2)
        for invalid in (
            "software-breakpoints: -1\n",
            "software-breakpoints: 0x2\n",
            report + "software-breakpoints: 3\n",
            "VitaDebugger status\n",
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(GATE.GateFailure):
                    GATE.breakpoint_count(invalid)

    def test_cleanup_sites_are_exact_current_fixture_encodings(self):
        self.assertEqual(
            GATE.FIXTURE_SITES,
            (
                ("step_target", 2, b"\x80\xb4"),
                ("thumb_step_exclusive_site", 4, b"\x50\xe8\x00\x1f"),
                ("thumb_step_exclusive_after", 2, b"\x00\x2a"),
                ("arm_step_exclusive_site", 4, b"\x9f\x1f\x90\xe1"),
                ("arm_step_exclusive_after", 4, b"\x00\x00\x52\xe3"),
            ),
        )
        self.assertEqual(
            GATE.TRAP_SITES,
            (
                ("thumb_step_exclusive_after", 2, b"\x00\x2a"),
                ("arm_step_exclusive_after", 4, b"\x00\x00\x52\xe3"),
            ),
        )
        self.assertEqual(GATE.UDF_BY_SIZE, {2: b"\x00\xde", 4: b"\xf0\x00\xf0\xe7"})

    def test_breakpoint_result_parser_rejects_missing_number(self):
        parsed = GATE.parse_breakpoint(
            '1^done,bkpt={number="7",type="breakpoint"}', "fixture"
        )
        self.assertEqual(parsed.number, "7")
        self.assertEqual(parsed.symbol, "fixture")
        with self.assertRaises(GATE.GateFailure):
            GATE.parse_breakpoint('1^done,bkpt={type="breakpoint"}', "fixture")

    def test_failed_mutation_readback_restores_before_error_propagates(self):
        pending = {}
        with (
            patch.object(
                GATE,
                "read_register",
                side_effect=[0x11111111, 0x33333333, 0x11111111],
            ),
            patch.object(GATE, "write_register") as write,
        ):
            with self.assertRaisesRegex(GATE.GateFailure, "read back"):
                GATE.mutate_and_restore_register(
                    object(), "r0", 0x22222222, pending
                )
        self.assertEqual(
            write.call_args_list,
            [call(ANY, "r0", 0x22222222), call(ANY, "r0", 0x11111111)],
        )
        self.assertEqual(pending, {})

    def test_mi_memory_read_requires_one_exact_sized_hex_payload(self):
        class FakeMi:
            token = 9

            def __init__(self, result):
                self.result = result

            def command(self, command):
                self.asserted_command = command
                return self.result, []

        valid = FakeMi(
            '9^done,memory=[{begin="0x81000000",offset="0x0",'
            'end="0x81000002",contents="80b4"}]'
        )
        self.assertEqual(GATE.read_gdb_memory(valid, 0x81000000, 2), b"\x80\xb4")
        self.assertEqual(
            valid.asserted_command, "-data-read-memory-bytes 0x81000000 2"
        )
        for malformed in (
            '9^done,memory=[{contents="80"}]',
            '9^done,memory=[{contents="zzzz"}]',
            '9^done,memory=[{contents="80b4"},{contents="80b4"}]',
        ):
            with self.subTest(malformed=malformed):
                with self.assertRaises(GATE.GateFailure):
                    GATE.read_gdb_memory(FakeMi(malformed), 0x81000000, 2)

    def test_five_site_preflight_fails_before_any_breakpoint_api(self):
        symbols = GATE.LiveSymbols(0x10, 0x20, 0x24, 0x100, 0x30, 0x34, 0x104)
        expected = [entry[2] for entry in GATE.FIXTURE_SITES]
        with patch.object(GATE, "read_gdb_memory", side_effect=expected) as read:
            observed = GATE.preflight_fixture_bytes(object(), symbols)
        self.assertEqual(len(observed), 5)
        self.assertEqual(read.call_count, 5)

        wrong = [b"\x00\x00", *expected[1:]]
        with patch.object(GATE, "read_gdb_memory", side_effect=wrong):
            with self.assertRaisesRegex(GATE.GateFailure, "do not match"):
                GATE.preflight_fixture_bytes(object(), symbols)

    def test_unrestored_mutation_path_kills_transport_without_detach(self):
        class FakeProcess:
            def __init__(self):
                self.killed = False
                self.waited = False

            def poll(self):
                return None

            def kill(self):
                self.killed = True

            def wait(self, timeout):
                self.waited = timeout > 0

        class FakeGdb:
            def __init__(self):
                self.process = FakeProcess()
                self.attached = True
                self.emergency_calls = 0

            def emergency_close(self):
                self.emergency_calls += 1

        client = FakeGdb()
        GATE.close_failed_gdb(client, True, 10.0)
        self.assertTrue(client.process.killed)
        self.assertTrue(client.process.waited)
        self.assertFalse(client.attached)
        self.assertEqual(client.emergency_calls, 0)

        clean = FakeGdb()
        GATE.close_failed_gdb(clean, False, 10.0)
        self.assertEqual(clean.emergency_calls, 1)
        self.assertFalse(clean.process.killed)

    def test_physical_udf_and_error_detach_gate_are_byte_exact(self):
        with patch.object(GATE, "read_memory", return_value=b"\x00\xde"):
            GATE.require_physical_udf(object(), "thumb", 0x1000, 2)
        with patch.object(GATE, "read_memory", return_value=b"\x00\xbe"):
            with self.assertRaisesRegex(GATE.GateFailure, "expected UDF"):
                GATE.require_physical_udf(object(), "thumb", 0x1000, 2)

        addresses = {
            "thumb_step_exclusive_after": 0x1000,
            "arm_step_exclusive_after": 0x2000,
        }
        originals = {
            "thumb_step_exclusive_after": "002a",
            "arm_step_exclusive_after": "000052e3",
        }
        with (
            patch.object(GATE, "raw_monitor_count", return_value=0),
            patch.object(GATE, "raw_sites_match", return_value=True),
        ):
            self.assertTrue(GATE.raw_safe_to_detach(object(), addresses, originals))
        for count, bytes_match in ((1, True), (0, False), (1, False)):
            with (
                patch.object(GATE, "raw_monitor_count", return_value=count),
                patch.object(GATE, "raw_sites_match", return_value=bytes_match),
            ):
                self.assertFalse(
                    GATE.raw_safe_to_detach(object(), addresses, originals)
                )

    def test_rsp_reconnect_retries_only_transient_peer_closure(self):
        class FakeRsp:
            def __init__(self, transient):
                self.transient = transient
                self.closed = False

            def negotiate(self):
                if self.transient:
                    raise GATE.monitor.SmokeFailure(
                        "debugger connection closed unexpectedly"
                    )

            def request(self, payload):
                self.requested = payload
                return b"T05thread:1;"

            def close(self):
                self.closed = True

        first, second = FakeRsp(True), FakeRsp(False)
        with (
            patch.object(GATE.monitor, "RspClient", side_effect=[first, second]),
            patch.object(GATE.time, "sleep"),
        ):
            connected = GATE.connect_rsp("vita.test", 1234, 1.0)
        self.assertIs(connected, second)
        self.assertTrue(first.closed)
        self.assertEqual(second.requested, b"?")

    def test_exclusive_step_reason_is_explicit(self):
        self.assertEqual(
            GATE.stop_reason('*stopped,reason="end-stepping-range",thread-id="1"'),
            "end-stepping-range",
        )
        with self.assertRaises(GATE.GateFailure):
            GATE.stop_reason('*stopped,thread-id="1"')


if __name__ == "__main__":
    unittest.main()
