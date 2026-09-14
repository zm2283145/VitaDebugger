import importlib.util
import io
import json
import queue
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "gdb_vfp_lifecycle.py"
SPEC = importlib.util.spec_from_file_location("gdb_vfp_lifecycle", TOOL)
assert SPEC and SPEC.loader
LIFECYCLE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LIFECYCLE
SPEC.loader.exec_module(LIFECYCLE)


class GdbVfpLifecycleTests(unittest.TestCase):
    def test_endpoint_validation_rejects_mi_injection_and_bad_ports(self):
        LIFECYCLE.validate_endpoint("192.0.2.10", 1234)
        for host in ("", "vita.local\n-gdb-exit", "host:1234"):
            with self.subTest(host=host):
                with self.assertRaises(LIFECYCLE.GateFailure):
                    LIFECYCLE.validate_endpoint(host, 1234)
        for port in (0, 65536):
            with self.subTest(port=port):
                with self.assertRaises(LIFECYCLE.GateFailure):
                    LIFECYCLE.validate_endpoint("vita.local", port)

    def test_mi_reader_rejects_an_overlong_line(self):
        client = object.__new__(LIFECYCLE.MiGdb)
        client.lines = queue.Queue()
        killed = []
        client.process = SimpleNamespace(
            stdout=io.StringIO("x" * (LIFECYCLE.MAX_MI_LINE_CHARACTERS + 1)),
            poll=lambda: None,
            kill=lambda: killed.append(True),
        )
        client._reader()
        failure = client.lines.get_nowait()
        self.assertIsInstance(failure, LIFECYCLE.GateFailure)
        self.assertIn("safety limit", str(failure))
        self.assertTrue(killed)
        client.lines.put(failure)
        client.timeout = 0.1
        client.transcript = []
        with self.assertRaisesRegex(LIFECYCLE.GateFailure, "safety limit"):
            client._next_line(LIFECYCLE.time.monotonic() + 0.1)

    def test_mi_strings_and_numeric_values_are_decoded_exactly(self):
        self.assertEqual(
            LIFECYCLE.decode_mi_string(r'"GDB VFP fixture\n"'),
            "GDB VFP fixture\n",
        )
        self.assertEqual(LIFECYCLE.parse_numeric("1.0"), Decimal(1))
        self.assertEqual(LIFECYCLE.parse_integer("0x00400000"), 0x00400000)
        with self.assertRaises(LIFECYCLE.GateFailure):
            LIFECYCLE.parse_integer("1.5")

    def test_fixture_selection_uses_name_and_is_foreign(self):
        record = (
            '7^done,threads=['
            '{id="1",target-id="Thread 0x40010003",name="test main",'
            'frame={level="0",func="main"}},'
            '{id="4",target-id="Thread 0x4001012b",'
            'details="GDB VFP fixture",frame={level="0",func="hold",'
            'args=[{name="pattern",value="0x81021028"},'
            '{name="ready",value="0x8102101c"}]}}],'
            'current-thread-id="1"'
        )
        selection = LIFECYCLE.parse_fixture_thread(record)
        self.assertEqual(selection.fixture_gdb_id, 4)
        self.assertEqual(selection.stopped_gdb_id, 1)
        self.assertEqual(selection.target_id, "Thread 0x4001012b")

    def test_fixture_must_not_be_the_exception_thread(self):
        record = (
            '8^done,threads=['
            '{id="4",target-id="Thread 0x4001012b",'
            'name="GDB VFP fixture"}],current-thread-id="4"'
        )
        with self.assertRaisesRegex(
            LIFECYCLE.GateFailure, "foreign-thread path was not tested"
        ):
            LIFECYCLE.parse_fixture_thread(record)

    def test_duplicate_or_missing_fixture_fails_closed(self):
        missing = (
            '9^done,threads=[{id="1",target-id="Thread 1",'
            'name="test main"}],current-thread-id="1"'
        )
        with self.assertRaisesRegex(LIFECYCLE.GateFailure, "found 0"):
            LIFECYCLE.parse_fixture_thread(missing)

        duplicate = (
            '10^done,threads=['
            '{id="2",target-id="Thread 2",name="GDB VFP fixture"},'
            '{id="3",target-id="Thread 3",name="GDB VFP fixture"}],'
            'current-thread-id="1"'
        )
        with self.assertRaisesRegex(LIFECYCLE.GateFailure, "found 2"):
            LIFECYCLE.parse_fixture_thread(duplicate)

    def test_progress_assertion_rejects_a_stalled_target(self):
        before = {"progress": 123}
        after = {"progress": 123}
        with self.assertRaisesRegex(LIFECYCLE.GateFailure, "did not change"):
            LIFECYCLE.require_progress(before, after, "reconnect")
        LIFECYCLE.require_progress(before, {"progress": 124}, "reconnect")

    def test_portable_session_evidence_contains_only_count_and_hash(self):
        transcript = [
            '> 4-target-select remote 192.0.2.10:1234',
            r'5^done,fullname="D:\\private\\checkout\\test.c"',
            '=library-loaded,id="PrivateModule",target-name="PrivateModule"',
        ]
        evidence = {"sessions": []}
        LIFECYCLE.record_session(evidence, "clean_detach", transcript, False)
        self.assertEqual(len(evidence["sessions"]), 1)
        session = evidence["sessions"][0]
        self.assertEqual(
            set(session), {"kind", "mi_record_count", "mi_sha256"}
        )
        self.assertEqual(session["mi_record_count"], len(transcript))
        self.assertEqual(len(session["mi_sha256"]), 64)

    def test_default_failure_evidence_never_copies_sensitive_text(self):
        secrets = (
            "192.0.2.10",
            r"D:\private\checkout\test.elf",
            "fullname=private.c",
            "PrivateModule",
        )
        transcript = [
            f"> -target-select remote {secrets[0]}:1234",
            f'~"Reading symbols from {secrets[1]}"',
            f'=library-loaded,id="{secrets[3]}"',
        ]
        evidence = {"sessions": []}
        LIFECYCLE.record_session(
            evidence, "first_session_failure", transcript, False
        )
        LIFECYCLE.record_failure(
            evidence,
            LIFECYCLE.GateFailure("; ".join(secrets)),
            False,
        )
        serialized = json.dumps(evidence)
        for secret in secrets:
            self.assertNotIn(secret, serialized)
        self.assertEqual(evidence["failure"]["type"], "GateFailure")

    def test_default_report_redacts_metadata_and_failure_on_disk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "private-checkout" / "test.elf"
            elf.parent.mkdir()
            elf.write_bytes(b"matching build")
            args = LIFECYCLE.argparse.Namespace(
                elf=elf,
                gdb=Path(r"D:\private-sdk\arm-vita-eabi-gdb.exe"),
                host="192.0.2.10",
                port=1234,
                include_sensitive_transcript=False,
            )
            evidence = LIFECYCLE.create_evidence(
                args, "GNU gdb test", "2026-01-01T00:00:00+00:00"
            )
            transcript = [
                f"> -target-select remote {args.host}:{args.port}",
                f'~"Reading symbols from {elf}"',
                '=library-loaded,id="PrivateModule"',
            ]
            LIFECYCLE.record_session(
                evidence, "first_session_failure", transcript, False
            )
            LIFECYCLE.record_failure(
                evidence,
                LIFECYCLE.GateFailure(f"failure at {elf} via {args.host}"),
                False,
            )
            output = root / "evidence.json"
            LIFECYCLE.write_evidence(output, evidence)
            serialized = output.read_text(encoding="utf-8")
            decoded_text = str(json.loads(serialized))
            self.assertNotIn(args.host, decoded_text)
            self.assertNotIn(str(elf), decoded_text)
            self.assertNotIn(str(args.gdb), decoded_text)
            self.assertNotIn("PrivateModule", decoded_text)
            self.assertNotIn('"mi"', serialized)
            self.assertIn('"mi_sha256"', serialized)

    def test_optional_hardware_metadata_is_portable_and_path_free(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "test.elf"
            plugin = root / "private-kernel-build" / "vitadebug.skprx"
            vpk = root / "private-package-build" / "uvdb-test.vpk"
            plugin.parent.mkdir()
            vpk.parent.mkdir()
            elf.write_bytes(b"elf")
            plugin.write_bytes(b"kernel companion")
            vpk.write_bytes(b"vita package")
            args = LIFECYCLE.argparse.Namespace(
                elf=elf,
                gdb=Path(r"D:\private-sdk\arm-vita-eabi-gdb.exe"),
                host="192.0.2.10",
                port=1234,
                include_sensitive_transcript=False,
                device_class="vita-tv",
                firmware="3.65",
                kernel_abi="v1.9",
                kernel_plugin=plugin,
                vpk=vpk,
            )
            evidence = LIFECYCLE.create_evidence(args, "GNU gdb test")
            self.assertEqual(
                evidence["hardware"]["device_class"], "vita-tv"
            )
            self.assertEqual(evidence["hardware"]["firmware"], "3.65")
            self.assertEqual(evidence["hardware"]["kernel_abi"], "v1.9")
            self.assertEqual(
                evidence["hardware"]["kernel_plugin"],
                {
                    "sha256": LIFECYCLE.sha256_file(plugin),
                    "size_bytes": plugin.stat().st_size,
                },
            )
            self.assertEqual(
                evidence["hardware"]["vpk"],
                {
                    "sha256": LIFECYCLE.sha256_file(vpk),
                    "size_bytes": vpk.stat().st_size,
                },
            )
            decoded_text = str(evidence)
            self.assertNotIn(str(plugin), decoded_text)
            self.assertNotIn(str(vpk), decoded_text)

    def test_optional_metadata_is_absent_for_legacy_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test.elf"
            elf.write_bytes(b"elf")
            args = LIFECYCLE.argparse.Namespace(
                elf=elf,
                gdb=Path("gdb"),
                host="127.0.0.1",
                port=1234,
                include_sensitive_transcript=False,
            )
            evidence = LIFECYCLE.create_evidence(args, "GNU gdb test")
            self.assertNotIn("hardware", evidence)

    def test_optional_metadata_and_artifact_inputs_fail_closed(self):
        invalid_values = ("", " padded", "trailing ", "x" * 65, "C:\\private")
        for value in invalid_values:
            with self.subTest(value=value):
                with self.assertRaises(LIFECYCLE.GateFailure):
                    LIFECYCLE.validate_portable_metadata("--firmware", value)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "secret-missing.skprx"
            with self.assertRaisesRegex(
                LIFECYCLE.GateFailure, "existing regular file"
            ) as missing_error:
                LIFECYCLE.portable_file_identity("--kernel-plugin", missing)
            self.assertNotIn(str(missing), str(missing_error.exception))
            with self.assertRaisesRegex(
                LIFECYCLE.GateFailure, "existing regular file"
            ):
                LIFECYCLE.portable_file_identity("--vpk", root)

    def test_sensitive_transcript_requires_explicit_opt_in(self):
        transcript = ['~"private full MI"']
        evidence = {"sessions": [], "sensitive": {"sessions": []}}
        LIFECYCLE.record_session(evidence, "failure", transcript, True)
        error = LIFECYCLE.GateFailure("private failure detail")
        LIFECYCLE.record_failure(evidence, error, True)
        self.assertEqual(evidence["sensitive"]["sessions"][0]["mi"], transcript)
        self.assertEqual(evidence["sensitive"]["error"], str(error))


if __name__ == "__main__":
    unittest.main()
