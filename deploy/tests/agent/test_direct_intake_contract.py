from __future__ import annotations

import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated C function {signature}")


class DirectIntakeAgentContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.direct = (PROJECT_ROOT / "agent" / "src" / "direct_intake.c").read_text(
            encoding="utf-8"
        )
        cls.main = (PROJECT_ROOT / "agent" / "src" / "main.c").read_text(
            encoding="utf-8"
        )
        cls.cmake = (PROJECT_ROOT / "agent" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        cls.build_script = (PROJECT_ROOT / "tools" / "build_agent.ps1").read_text(
            encoding="utf-8"
        )

    def test_direct_transport_is_opt_in_and_ftp_path_remains(self) -> None:
        self.assertIn('option(VDEV_ENABLE_DIRECT_TCP', self.cmake)
        self.assertIn('"Enable the experimental authenticated direct TCP package intake" OFF', self.cmake)
        self.assertIn('VDEV_DIRECT_TCP_PORT "18196"', self.cmake)
        self.assertIn("[int] $DirectTcpPort = 18196", self.build_script)
        self.assertIn('"-DVDEV_DIRECT_TCP_PORT=$DirectTcpPort"', self.build_script)
        self.assertIn("vdev_find_committed_job(job_id, job_directory)", self.main)
        self.assertIn("vdev_direct_poll(&direct_server, nonce)", self.main)

    def test_build_rejects_non_subgroup_trust_keys_and_records_fingerprint(self) -> None:
        self.assertIn("validate_ed25519_public_key.py", self.cmake)
        self.assertIn("VDD_PUBLIC_KEY_VALIDATION_RESULT", self.cmake)
        self.assertIn("VDD_PUBLIC_KEY_SHA256_LENGTH", self.cmake)
        self.assertIn("vdd-public-key-fingerprint.txt", self.cmake)
        self.assertIn("vdd-public-key-fingerprint.txt", self.build_script)
        self.assertIn(
            "$validatedPublicKeyFingerprint -cne $publicKeyFingerprint",
            self.build_script,
        )
        self.assertIn(
            '"trusted_public_key_fingerprint=sha256:$validatedPublicKeyFingerprint"',
            self.build_script,
        )
        self.assertIn('"private_key_embedded=no"', self.build_script)
        self.assertNotIn("PrivateKeyHex", self.build_script)

    def test_metadata_authentication_precedes_payload_ack_and_file_receive(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        signature = "vdev_verify_job_signature(signature, request_data,"
        authenticated_ack = (
            "send_ack(socket_id, VDEV_DIRECT_ACK_AUTHENTICATED,\n"
            "                      VDEV_OK, 0u, 0u)"
        )
        self.assertIn(signature, handler)
        self.assertIn(authenticated_ack, handler)
        self.assertIn("receive_file(socket_id, path", handler)
        self.assertLess(handler.index(signature), handler.index(authenticated_ack))
        self.assertLess(handler.index(signature), handler.index("vdev_parse_manifest("))
        self.assertLess(handler.index(authenticated_ack), handler.index("receive_file(socket_id, path"))

    def test_preauthentication_input_has_one_absolute_deadline(self) -> None:
        receiver = _function(self.direct, "static int receive_exact_until(")
        handler = _function(self.direct, "static int handle_client(")
        self.assertIn("VDEV_DIRECT_PREAUTH_TIMEOUT_US", self.direct)
        self.assertIn("absolute_deadline", receiver)
        self.assertIn("now >= absolute_deadline", receiver)
        self.assertLess(
            receiver.index("now >= absolute_deadline"),
            receiver.index("sceNetRecv("),
        )
        self.assertEqual(handler.count("preauth_deadline"), 6)
        self.assertEqual(
            handler.count("receive_exact_until(socket_id"),
            4,
        )

    def test_authentication_failures_keep_specific_diagnostics(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        self.assertIn("result = VDEV_ERR_SIGNATURE;", handler)
        self.assertIn("result = VDEV_ERR_HASH;", handler)
        self.assertIn("result = VDEV_ERR_FORMAT;", handler)
        self.assertNotIn("if (result >= 0) result = VDEV_ERR_SIGNATURE;", handler)

    def test_request_file_remains_the_only_commit_point(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        receive = handler.index("receive_file(socket_id, path")
        commit = handler.index(
            "vdev_write_atomic(request_path, request_data, request_size"
        )
        committed_ack = handler.index(
            "send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, VDEV_OK"
        )
        self.assertLess(receive, commit)
        self.assertLess(commit, committed_ack)

    def test_known_commit_waits_for_a_checked_device_barrier(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        commit = handler.index(
            "vdev_write_atomic(request_path, request_data, request_size"
        )
        barrier = handler.index("result = vdev_sync_device();", commit)
        committed_ack = handler.index(
            "send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, VDEV_OK"
        )
        self.assertLess(commit, barrier)
        self.assertLess(barrier, committed_ack)
        self.assertIn("result = VDEV_ERR_COMMIT_UNKNOWN;", handler[barrier:committed_ack])

    def test_replay_paths_are_never_cleaned_as_uncommitted_retries(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        replay = handler.index("result = VDEV_ERR_REPLAY;")
        ownership = handler.index(
            "vdev_direct_transaction_take_staging(&transaction);"
        )
        cleanup_gate = handler.index("reject_owned_auth:")
        self.assertLess(replay, ownership)
        self.assertLess(ownership, cleanup_gate)

    def test_interrupted_payload_is_cleaned_without_consuming_challenge(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        reject_payload = handler[handler.index("reject_payload:") :]
        self.assertIn(
            "reset_uncommitted_job(job_directory, &manifest, 1)", reject_payload
        )
        self.assertLess(
            reject_payload.index(
                "reset_uncommitted_job(job_directory, &manifest, 1)"
            ),
            reject_payload.index("send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED"),
        )
        self.assertIn(
            "vdev_direct_transaction_failure(&transaction, cleanup_result)",
            reject_payload,
        )
        self.assertIn("result = VDEV_ERR_COMMIT_UNKNOWN;", reject_payload)
        self.assertNotIn("vdev_consume_challenge", self.direct)

    def test_post_rename_errors_are_ambiguous_and_never_cleaned(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        commit = handler[handler.index("record_request_commit") : handler.index("reject_payload:")]
        self.assertIn("vdev_direct_transaction_failure(&transaction", commit)
        self.assertIn("result = VDEV_ERR_COMMIT_UNKNOWN;", commit)
        self.assertNotIn("reset_uncommitted_job", commit)
        trace = _function(self.direct, "static void record_request_commit(")
        self.assertIn("step == VDEV_ATOMIC_STEP_RENAME", trace)
        self.assertIn("event == VDEV_ATOMIC_TRACE_RESULT", trace)
        self.assertIn("code >= 0", trace)
        self.assertIn(
            "vdev_direct_transaction_record_request_rename(transaction)", trace
        )

    def test_confirmed_cleanup_is_synced_before_definite_failure_ack(self) -> None:
        reset = _function(self.direct, "static int reset_uncommitted_job(")
        prove = _function(self.direct, "static int prove_job_directory_absent(")
        observe = _function(self.direct, "static int observe_job_directory(")
        self.assertIn("prove_job_directory_absent(job_directory)", reset)
        self.assertIn("vdev_sync_parent_directory(job_directory)", prove)
        self.assertIn("observe_job_directory(job_directory, &observation)", prove)
        self.assertIn("VDEV_DIRECT_PATH_MISSING", prove)
        self.assertIn("sceIoGetstat(job_directory, &stat)", observe)
        self.assertIn("VDEV_DIRECT_PATH_IO_ERROR", observe)
        self.assertIn("VDEV_DIRECT_PATH_OTHER", observe)

    def test_every_owned_preauth_failure_cleans_before_reply_or_return(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        owned = handler[handler.index("reject_owned_auth:") : handler.index("reject_auth:")]
        auth_cleanup = owned.index(
            "reset_uncommitted_job(job_directory, &manifest, 1)"
        )
        auth_ack = owned.index(
            "send_ack(socket_id, VDEV_DIRECT_ACK_AUTHENTICATED"
        )
        self.assertLess(auth_cleanup, auth_ack)
        no_reply = owned[owned.index("reject_owned_no_reply:") :]
        self.assertIn(
            "reset_uncommitted_job(job_directory, &manifest, 1)", no_reply
        )
        self.assertNotIn("send_ack(", no_reply)
        self.assertIn("if (result < 0) goto reject_owned_no_reply;", handler)
        self.assertIn("goto reject_owned_auth;", handler)

    def test_cleanup_uncertainty_uses_commit_unknown_in_both_ack_phases(self) -> None:
        handler = _function(self.direct, "static int handle_client(")
        self.assertGreaterEqual(handler.count("VDEV_ERR_COMMIT_UNKNOWN"), 4)
        self.assertIn("VDEV_DIRECT_ACK_AUTHENTICATED", handler)
        self.assertIn("VDEV_DIRECT_ACK_COMMITTED", handler)


if __name__ == "__main__":
    unittest.main()
