import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import gdb_symbols as symbols  # noqa: E402


class FakeSocket:
    def __init__(self, *, fail_send=False):
        self.fail_send = fail_send
        self.timeouts = []
        self.closed = False
        self.send_count = 0

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def sendall(self, _data):
        self.send_count += 1
        if self.fail_send:
            raise ConnectionResetError("peer reset after TCP accept")

    def close(self):
        self.closed = True


class ConnectWaitTests(unittest.TestCase):
    def test_retries_connect_failures_and_keeps_first_successful_socket(self):
        connected = FakeSocket()
        attempts = [
            ConnectionRefusedError("not listening yet"),
            TimeoutError("launch still pending"),
            connected,
        ]
        with (
            mock.patch.object(
                symbols.socket, "create_connection", side_effect=attempts
            ) as create_connection,
            mock.patch.object(symbols.time, "sleep") as sleep,
        ):
            client = symbols.RspClient(
                "192.0.2.10", 1234, timeout=4.0, connect_wait=2.0
            )

        self.assertIs(client.socket, connected)
        self.assertEqual(create_connection.call_count, 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual(connected.timeouts, [4.0])
        self.assertFalse(connected.closed)
        for call in create_connection.call_args_list:
            self.assertEqual(call.args, (("192.0.2.10", 1234),))
            self.assertGreater(call.kwargs["timeout"], 0)
            self.assertLessEqual(call.kwargs["timeout"], 2.0)
        client.close()
        self.assertTrue(connected.closed)

    def test_default_has_one_immediate_connection_attempt(self):
        failure = ConnectionRefusedError("not listening")
        with (
            mock.patch.object(
                symbols.socket, "create_connection", side_effect=failure
            ) as create_connection,
            mock.patch.object(symbols.time, "sleep") as sleep,
        ):
            with self.assertRaises(ConnectionRefusedError):
                symbols.RspClient("192.0.2.10", 1234, timeout=4.0)

        create_connection.assert_called_once_with(
            ("192.0.2.10", 1234), timeout=4.0
        )
        sleep.assert_not_called()

    def test_deadline_stops_before_an_extra_connection_attempt(self):
        failure = ConnectionRefusedError("not listening")
        with (
            mock.patch.object(
                symbols.socket, "create_connection", side_effect=failure
            ) as create_connection,
            mock.patch.object(
                symbols.time,
                "monotonic",
                side_effect=(100.0, 100.0, 100.25, 101.1),
            ),
            mock.patch.object(symbols.time, "sleep") as sleep,
        ):
            with self.assertRaisesRegex(
                symbols.SymbolError, "did not become ready within 1 seconds"
            ):
                symbols.RspClient(
                    "192.0.2.10", 1234, timeout=4.0, connect_wait=1.0
                )

        self.assertEqual(create_connection.call_count, 1)
        sleep.assert_called_once_with(symbols.CONNECT_RETRY_DELAY)

    def test_protocol_failure_after_connect_is_never_retried(self):
        connected = FakeSocket(fail_send=True)
        with (
            mock.patch.object(
                symbols.socket, "create_connection", return_value=connected
            ) as create_connection,
            mock.patch.object(symbols.time, "sleep") as sleep,
        ):
            with self.assertRaisesRegex(
                ConnectionResetError, "peer reset after TCP accept"
            ):
                symbols.query_target(
                    "192.0.2.10", 1234, timeout=4.0, connect_wait=30.0
                )

        create_connection.assert_called_once()
        sleep.assert_not_called()
        self.assertEqual(connected.send_count, 2)
        self.assertTrue(connected.closed)

    def test_wait_is_bounded_and_exposed_by_the_cli(self):
        parser = symbols.build_argument_parser()
        args = parser.parse_args([
            "--main-elf", "game.elf",
            "--host", "192.0.2.10",
            "--allow-unverified-build",
            "--connect-wait", "12.5",
        ])
        self.assertEqual(args.connect_wait, 12.5)
        with self.assertRaisesRegex(symbols.SymbolError, "between 0 and 60"):
            symbols._connect_rsp_socket(
                "192.0.2.10", 1234, timeout=4.0, connect_wait=60.01
            )


if __name__ == "__main__":
    unittest.main()
