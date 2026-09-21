import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from tools.pmu_disconnect_receiver import PRELUDE, run_receiver


class DisconnectReceiverTests(unittest.TestCase):
    def test_accepts_exact_prelude_and_records_reset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "receiver.json"
            ready = threading.Event()
            result = {}

            def server() -> None:
                with socket.socket(
                    socket.AF_INET, socket.SOCK_STREAM
                ) as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                result["port"] = port
                ready.set()
                result["evidence"] = run_receiver(
                    "127.0.0.1", port, evidence, 2.0
                )

            thread = threading.Thread(target=server)
            thread.start()
            self.assertTrue(ready.wait(1.0))
            with socket.create_connection(
                ("127.0.0.1", result["port"]), timeout=2.0
            ) as client:
                client.sendall(PRELUDE)
            thread.join(3.0)
            self.assertFalse(thread.is_alive())
            written = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(written["state"], "reset_sent")
            self.assertEqual(written["prelude_hex"], PRELUDE.hex())
            self.assertEqual(result["evidence"]["state"], "reset_sent")


if __name__ == "__main__":
    unittest.main()
