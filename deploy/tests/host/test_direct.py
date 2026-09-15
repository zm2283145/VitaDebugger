from __future__ import annotations

import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.direct import (
    ACK_AUTHENTICATED,
    ACK_COMMITTED,
    DEFAULT_DIRECT_PORT,
    DirectCommitState,
    DirectTransferError,
    VDEV_ERR_COMMIT_UNKNOWN,
    VDEV_ERR_REPLAY,
    VitaDirectClient,
    decode_greeting,
    decode_job_header,
    encode_ack,
    encode_greeting,
    prepare_direct_transfer,
)
from vitadevdeploy.errors import DeploymentError, ProtocolError
from vitadevdeploy.job import load_prepared_job
from vitadevdeploy.manifest import build_manifest
from vitadevdeploy.protocol import make_request

try:
    from .helpers import make_sfo
except ImportError:
    from helpers import make_sfo


NONCE = "ab" * 32


def _recv_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = connection.recv(size - len(result))
        if not block:
            raise EOFError("peer closed")
        result.extend(block)
    return bytes(result)


def _make_job(root: Path) -> object:
    job_id = "12" * 16
    job_root = root / job_id
    package = job_root / "package"
    (package / "sce_sys" / "package").mkdir(parents=True)
    (package / "eboot.bin").write_bytes(b"eboot-payload")
    (package / "sce_sys" / "param.sfo").write_bytes(make_sfo())
    (package / "sce_sys" / "package" / "head.bin").write_bytes(b"head-payload")
    manifest = build_manifest(package)
    request = make_request(
        job=job_id,
        nonce=NONCE,
        action="verify",
        title_id="TEST00001",
        manifest_sha256=manifest.sha256,
        file_count=manifest.file_count,
        total_size=manifest.total_size,
    )
    (job_root / "manifest.v1").write_bytes(manifest.data)
    (job_root / "request.v1").write_bytes(request.data)
    (job_root / "signature.bin").write_bytes(b"s" * 64)
    return load_prepared_job(job_root)


class _Server:
    def __init__(self, handler: object) -> None:
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.error: BaseException | None = None

        def serve() -> None:
            try:
                connection, _ = self.listener.accept()
                with connection:
                    handler(connection)  # type: ignore[operator]
            except BaseException as exc:  # surfaced in join()
                self.error = exc
            finally:
                self.listener.close()

        self.thread = threading.Thread(target=serve, daemon=True)
        self.thread.start()

    def join(self) -> None:
        self.thread.join(3)
        if self.thread.is_alive():
            self.fail("direct protocol test server did not exit")
        if self.error is not None:
            raise self.error

    def fail(self, message: str) -> None:
        raise AssertionError(message)


class DirectProtocolTests(unittest.TestCase):
    def test_default_port_is_outside_existing_companion_ports(self) -> None:
        self.assertEqual(DEFAULT_DIRECT_PORT, 18196)
        self.assertNotIn(DEFAULT_DIRECT_PORT, {1234, 1337, 1338, 18194, 18195})

    def test_greeting_is_fixed_size_and_canonical(self) -> None:
        encoded = encode_greeting(NONCE)
        self.assertEqual(len(encoded), 40)
        self.assertEqual(decode_greeting(encoded).nonce, NONCE)
        with self.assertRaises(ProtocolError):
            encode_greeting(NONCE.upper())
        with self.assertRaises(ProtocolError):
            decode_greeting(b"wrong")

    def test_job_header_rejects_oversized_or_invalid_metadata(self) -> None:
        with self.assertRaises(ProtocolError):
            decode_job_header(struct.pack("!8sIIIQ", b"VDDJOB01", 0, 1, 64, 0))
        with self.assertRaises(ProtocolError):
            decode_job_header(
                struct.pack("!8sIIIQ", b"VDDJOB01", 1, 4 * 1024 * 1024 + 1, 64, 0)
            )
        with self.assertRaises(ProtocolError):
            decode_job_header(struct.pack("!8sIIIQ", b"VDDJOB01", 1, 1, 63, 0))

    def test_short_greeting_closes_the_partial_connection(self) -> None:
        def handler(connection: socket.socket) -> None:
            connection.sendall(b"VDDCHL01short")

        server = _Server(handler)
        client = VitaDirectClient("127.0.0.1", port=server.port, timeout=2)
        with self.assertRaises(DeploymentError):
            client.connect()
        with self.assertRaisesRegex(DeploymentError, "not connected"):
            _ = client.connection
        server.join()

    def test_v2_greeting_fails_closed_instead_of_being_parsed_as_v1(self) -> None:
        def handler(connection: socket.socket) -> None:
            connection.sendall(b"VDDCHL02" + bytes.fromhex(NONCE))
            connection.settimeout(2)
            self.assertEqual(connection.recv(1), b"")

        server = _Server(handler)
        client = VitaDirectClient("127.0.0.1", port=server.port, timeout=2)
        with self.assertRaisesRegex(DeploymentError, "greeting magic is invalid"):
            client.connect()
        with self.assertRaisesRegex(DeploymentError, "not connected"):
            _ = client.connection
        server.join()

    def test_metadata_is_authenticated_before_any_package_bytes_are_sent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)
            observed: dict[str, object] = {}

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                header = _recv_exact(connection, 28)
                request_size, manifest_size, signature_size, package_size = decode_job_header(header)
                observed["request"] = _recv_exact(connection, request_size)
                observed["manifest"] = _recv_exact(connection, manifest_size)
                observed["signature"] = _recv_exact(connection, signature_size)
                observed["package_size"] = package_size

                connection.settimeout(0.08)
                with self.assertRaises(socket.timeout):
                    connection.recv(1)
                connection.settimeout(2)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                payload = _recv_exact(connection, package_size)
                observed["payload"] = payload
                connection.sendall(
                    encode_ack(
                        ACK_COMMITTED,
                        0,
                        job.manifest.file_count,
                        job.manifest.total_size,
                    )
                )

            server = _Server(handler)
            client = VitaDirectClient("127.0.0.1", port=server.port, timeout=2)
            with client:
                receipt = client.stage_job(plan)
            server.join()

            expected_payload = b"".join(
                job.package.joinpath(*entry.path.split("/")).read_bytes()
                for entry in job.manifest.entries
            )
            self.assertEqual(observed["request"], job.request_path.read_bytes())
            self.assertEqual(observed["manifest"], job.manifest_path.read_bytes())
            self.assertEqual(observed["signature"], b"s" * 64)
            self.assertEqual(observed["payload"], expected_payload)
            self.assertEqual(receipt.resumed_files, 0)
            self.assertEqual(receipt.committed_files, job.manifest.file_count)
            self.assertEqual(
                receipt.commit_state, DirectCommitState.AMBIGUOUS_AFTER_COMMIT
            )

    def test_connected_stage_requires_an_offline_transfer_plan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            client = VitaDirectClient("127.0.0.1")
            with self.assertRaisesRegex(TypeError, "offline DirectTransferPlan"):
                client.stage_job(job)  # type: ignore[arg-type]

    def test_resume_is_limited_to_a_complete_manifest_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)
            first_size = job.manifest.entries[0].size
            remaining_size = job.manifest.total_size - first_size
            received = bytearray()

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, package_size = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                self.assertEqual(package_size, job.manifest.total_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 1, first_size))
                received.extend(_recv_exact(connection, remaining_size))
                connection.sendall(
                    encode_ack(
                        ACK_COMMITTED,
                        0,
                        job.manifest.file_count,
                        job.manifest.total_size,
                    )
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                receipt = client.stage_job(plan)
            server.join()

            expected = b"".join(
                job.package.joinpath(*entry.path.split("/")).read_bytes()
                for entry in job.manifest.entries[1:]
            )
            self.assertEqual(bytes(received), expected)
            self.assertEqual(receipt.resumed_files, 1)
            self.assertEqual(receipt.resumed_bytes, first_size)

    def test_non_boundary_resume_is_rejected_without_sending_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 1, 999999))
                connection.settimeout(0.15)
                try:
                    extra = connection.recv(1)
                except (socket.timeout, ConnectionResetError):
                    extra = b""
                self.assertEqual(extra, b"")

            server = _Server(handler)
            client = VitaDirectClient("127.0.0.1", port=server.port, timeout=2)
            with client:
                with self.assertRaisesRegex(
                    DirectTransferError, "manifest boundary"
                ) as captured:
                    client.stage_job(plan)
                self.assertEqual(
                    captured.exception.commit_state,
                    DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                )
            server.join()

    def test_interrupted_stream_never_returns_a_commit_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                _recv_exact(connection, 1)
                connection.shutdown(socket.SHUT_RDWR)

            server = _Server(handler)
            client = VitaDirectClient("127.0.0.1", port=server.port, timeout=2)
            with client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
                self.assertEqual(
                    captured.exception.commit_state,
                    DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
                )
            server.join()

    def test_unauthenticated_metadata_rejection_never_discards_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, -20008, 0, 0))

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaisesRegex(DeploymentError, "-20008") as captured:
                    client.stage_job(plan)
                self.assertEqual(
                    captured.exception.commit_state,
                    DirectCommitState.FAIL_BEFORE_COMMIT,
                )
            server.join()

    def test_v2_phase_one_ack_fails_closed_without_sending_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(
                    struct.pack(
                        "!8sIiIQ", b"VDDACK02", ACK_AUTHENTICATED, 0, 0, 0
                    )
                )
                connection.settimeout(2)
                self.assertEqual(connection.recv(1), b"")

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.FAIL_BEFORE_COMMIT,
            )
            self.assertFalse(captured.exception.result_may_appear)

    def test_phase_one_cleanup_uncertainty_retains_evidence_without_result_poll(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(
                    encode_ack(ACK_AUTHENTICATED, VDEV_ERR_COMMIT_UNKNOWN, 0, 0)
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )
            self.assertFalse(captured.exception.result_may_appear)

    def test_unauthenticated_payload_rejection_is_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, package_size = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                _recv_exact(connection, package_size)
                connection.sendall(encode_ack(ACK_COMMITTED, -20005, 0, 0))

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )
            self.assertTrue(captured.exception.result_may_appear)

    def test_v2_phase_two_ack_fails_closed_as_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, package_size = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                _recv_exact(connection, package_size)
                connection.sendall(
                    struct.pack(
                        "!8sIiIQ", b"VDDACK02", ACK_COMMITTED, 0, 0, 0
                    )
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )
            self.assertTrue(captured.exception.result_may_appear)

    def test_replay_ack_is_reconciled_as_a_possible_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, _ = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(
                    encode_ack(ACK_AUTHENTICATED, VDEV_ERR_REPLAY, 0, 0)
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )
            self.assertTrue(captured.exception.result_may_appear)

    def test_phase_two_replay_ack_is_reconciled_as_a_possible_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, package_size = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                _recv_exact(connection, package_size)
                connection.sendall(
                    encode_ack(ACK_COMMITTED, VDEV_ERR_REPLAY, 0, 0)
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )
            self.assertTrue(captured.exception.result_may_appear)

    def test_receiver_commit_uncertainty_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            job = _make_job(Path(directory))
            plan = prepare_direct_transfer(job)

            def handler(connection: socket.socket) -> None:
                connection.sendall(encode_greeting(NONCE))
                request_size, manifest_size, signature_size, package_size = decode_job_header(
                    _recv_exact(connection, 28)
                )
                _recv_exact(connection, request_size + manifest_size + signature_size)
                connection.sendall(encode_ack(ACK_AUTHENTICATED, 0, 0, 0))
                _recv_exact(connection, package_size)
                connection.sendall(
                    encode_ack(
                        ACK_COMMITTED,
                        VDEV_ERR_COMMIT_UNKNOWN,
                        job.manifest.file_count,
                        job.manifest.total_size,
                    )
                )

            server = _Server(handler)
            with VitaDirectClient("127.0.0.1", port=server.port, timeout=2) as client:
                with self.assertRaises(DirectTransferError) as captured:
                    client.stage_job(plan)
            server.join()
            self.assertEqual(
                captured.exception.commit_state,
                DirectCommitState.AMBIGUOUS_AFTER_COMMIT,
            )


if __name__ == "__main__":
    unittest.main()
