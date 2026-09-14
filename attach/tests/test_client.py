from __future__ import annotations

import socket
import sys
import threading
import time
import unittest
from pathlib import Path


ATTACH_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ATTACH_ROOT / "host"))

from vdattach.client import (  # noqa: E402
    AttachDiscoveryClient,
    canonical_ipv4,
    checked_port,
    receive_record,
    send_record,
)
from vdattach.cli import build_parser  # noqa: E402
from vdattach.errors import (  # noqa: E402
    CapabilityError,
    ProtocolError,
    TransportError,
)
from vdattach.protocol import (  # noqa: E402
    CURRENT_KERNEL_ABI,
    READ_ONLY_CAPABILITIES,
    DiscoverResult,
    HelloResult,
    ReleaseResult,
    parse_discover_request,
    parse_hello_request,
    parse_release_request,
)


SERVER_NONCE = "33" * 32
TARGET_TICKET = "44" * 32
SERVICE_GENERATION = 0x1020304050607080
TARGET_GENERATION = 0x1122334455667788


class FakeBroker:
    def __init__(
        self,
        stream: socket.socket,
        *,
        caps: int = READ_ONLY_CAPABILITIES,
        kernel_abi: int = CURRENT_KERNEL_ABI,
        response_client_nonce: str | None = None,
        hello_only: bool = False,
    ):
        self.stream = stream
        self.stream.settimeout(2.0)
        self.caps = caps
        self.kernel_abi = kernel_abi
        self.response_client_nonce = response_client_nonce
        self.hello_only = hello_only
        self.error: BaseException | None = None
        self.messages: list[str] = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def join(self) -> None:
        self.thread.join(timeout=3.0)
        if self.thread.is_alive():
            self.fail("fake broker did not finish")
        if self.error is not None:
            raise self.error

    def fail(self, message: str) -> None:
        raise AssertionError(message)

    def _run(self) -> None:
        try:
            hello = parse_hello_request(receive_record(self.stream))
            self.messages.append("hello")
            send_record(
                self.stream,
                HelloResult(
                    request_id=hello.request_id,
                    client_nonce=self.response_client_nonce or hello.client_nonce,
                    server_nonce=SERVER_NONCE,
                    service_generation=SERVICE_GENERATION,
                    state="ready",
                    attach_caps=self.caps,
                    kernel_abi=self.kernel_abi,
                    kernel_caps=0x0000000F,
                    ticket_lease_ms=5000,
                ).data,
            )
            if self.hello_only or self.caps != READ_ONLY_CAPABILITIES:
                return

            discover = parse_discover_request(receive_record(self.stream))
            self.messages.append("discover")
            send_record(
                self.stream,
                DiscoverResult(
                    request_id=discover.request_id,
                    server_nonce=discover.server_nonce,
                    service_generation=discover.service_generation,
                    state="found",
                    target_title_id=discover.target_title_id,
                    pid=0x10005,
                    main_modid=0x40001234,
                    main_fingerprint=0xAABBCCDD,
                    target_generation=TARGET_GENERATION,
                    target_ticket=TARGET_TICKET,
                ).data,
            )

            release = parse_release_request(receive_record(self.stream))
            self.messages.append("release")
            send_record(
                self.stream,
                ReleaseResult(
                    request_id=release.request_id,
                    server_nonce=release.server_nonce,
                    service_generation=release.service_generation,
                    target_ticket=release.target_ticket,
                    state="released",
                ).data,
            )
        except BaseException as exc:
            self.error = exc
        finally:
            self.stream.close()


class ClientTests(unittest.TestCase):
    def test_read_only_state_machine_discovers_exact_title_then_releases(self):
        client_socket, server_socket = socket.socketpair()
        client_socket.settimeout(2.0)
        broker = FakeBroker(server_socket)
        broker.start()
        try:
            with AttachDiscoveryClient(client_socket) as client:
                hello = client.hello()
                self.assertEqual(hello.kernel_abi, CURRENT_KERNEL_ABI)
                snapshot = client.discover("UVDBDEMO1")
                self.assertEqual(snapshot.pid, 0x10005)
                self.assertEqual(snapshot.main_fingerprint, 0xAABBCCDD)
                with self.assertRaisesRegex(ProtocolError, "release the existing"):
                    client.discover("UVDBDEMO1")
                released = client.release()
                self.assertEqual(released.state, "released")
        finally:
            broker.join()
        self.assertEqual(broker.messages, ["hello", "discover", "release"])

    def test_missing_capability_stops_before_title_discovery(self):
        client_socket, server_socket = socket.socketpair()
        client_socket.settimeout(2.0)
        broker = FakeBroker(server_socket, caps=READ_ONLY_CAPABILITIES & ~(1 << 2))
        broker.start()
        try:
            with AttachDiscoveryClient(client_socket) as client:
                with self.assertRaisesRegex(CapabilityError, "lacks required"):
                    client.hello()
        finally:
            broker.join()
        self.assertEqual(broker.messages, ["hello"])

    def test_handshake_rejects_wrong_nonce_and_kernel_abi(self):
        cases = (
            ({"response_client_nonce": "55" * 32}, ProtocolError, "not bound"),
            ({"kernel_abi": CURRENT_KERNEL_ABI + 1}, CapabilityError, "ABI mismatch"),
        )
        for options, error_type, message in cases:
            with self.subTest(options=options):
                client_socket, server_socket = socket.socketpair()
                client_socket.settimeout(2.0)
                broker = FakeBroker(server_socket, hello_only=True, **options)
                broker.start()
                try:
                    with AttachDiscoveryClient(client_socket) as client:
                        with self.assertRaisesRegex(error_type, message):
                            client.hello()
                finally:
                    broker.join()
                self.assertEqual(broker.messages, ["hello"])

    def test_discovery_requires_handshake_and_one_active_ticket(self):
        client_socket, server_socket = socket.socketpair()
        server_socket.close()
        with AttachDiscoveryClient(client_socket) as client:
            with self.assertRaisesRegex(ProtocolError, "hello must succeed"):
                client.discover("UVDBDEMO1")
            with self.assertRaisesRegex(ProtocolError, "no discovery ticket"):
                client.release()

    def test_network_endpoint_validation_rejects_dns_and_bad_ports(self):
        self.assertEqual(canonical_ipv4("192.0.2.10"), "192.0.2.10")
        for host in ("vita.local", "192.168.001.2", "127.0.0.1 ", ""):
            with self.subTest(host=host):
                with self.assertRaises(ProtocolError):
                    canonical_ipv4(host)
        for port in (0, -1, 65536, True):
            with self.subTest(port=port):
                with self.assertRaises(ProtocolError):
                    checked_port(port)

    def test_frame_receive_has_one_absolute_exchange_deadline(self):
        client_socket, server_socket = socket.socketpair()
        with client_socket, server_socket:
            server_socket.sendall(b"\x00")
            start = time.monotonic()
            with self.assertRaisesRegex(TransportError, "time limit"):
                receive_record(client_socket, start + 0.05)
            self.assertLess(time.monotonic() - start, 0.5)

    def test_cli_exposes_no_pid_path_or_mutating_operation(self):
        parser = build_parser()
        command_action = next(action for action in parser._actions if action.dest == "command")
        self.assertEqual(set(command_action.choices), {"status", "discover"})
        for command in command_action.choices.values():
            destinations = {action.dest for action in command._actions}
            self.assertNotIn("pid", destinations)
            self.assertNotIn("module_path", destinations)
            self.assertNotIn("address", destinations)


if __name__ == "__main__":
    unittest.main()
