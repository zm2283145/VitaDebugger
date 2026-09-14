import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
TOOL = TOOLS / "gdb_monitor_smoke.py"
SPEC = importlib.util.spec_from_file_location("gdb_monitor_smoke", TOOL)
assert SPEC and SPEC.loader
MONITOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MONITOR
SPEC.loader.exec_module(MONITOR)


class MonitorSmokeTests(unittest.TestCase):
    def test_ok_is_not_mistaken_for_console_output(self):
        self.assertFalse(MONITOR.is_console_output_packet(b"OK"))
        self.assertTrue(MONITOR.is_console_output_packet(b"O6869"))
        self.assertFalse(MONITOR.is_console_output_packet(b""))

    def test_async_console_demux_stops_at_ok(self):
        client = object.__new__(MONITOR.RspClient)
        client.console_output = bytearray()
        packets = iter((b"O6869", b"OK"))
        client.read_packet = lambda: next(packets)

        self.assertEqual(client.read_response(), b"OK")
        self.assertEqual(client.console_output, b"hi")

    def test_monitor_request_keeps_its_output_and_accepts_ok(self):
        class FakeClient:
            def __init__(self):
                self.request_demux = None

            def request(self, payload, *, demux_console=True):
                self.payload = payload
                self.request_demux = demux_console
                return b"O7374617475730a"

            def read_packet(self):
                return b"OK"

        client = FakeClient()
        self.assertEqual(MONITOR.monitor_request(client, "status"), "status\n")
        self.assertFalse(client.request_demux)
        self.assertEqual(client.payload, b"qRcmd,737461747573")


if __name__ == "__main__":
    unittest.main()
