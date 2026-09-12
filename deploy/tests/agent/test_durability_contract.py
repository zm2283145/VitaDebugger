from __future__ import annotations

import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
AGENT_SOURCE = PROJECT_ROOT / "agent" / "src"
DISPOSABLE_SOURCE = PROJECT_ROOT / "disposable-target" / "src" / "main.c"
DEVICE_SYNC_CALL = "sceIoSync(VDEV_STORAGE_DEVICE, 0)"
EACCES_HEX = "0x8001000D"


def _function(source: str, signature: str) -> str:
    """Return one C function body, including its outer braces."""

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
    raise AssertionError(f"unterminated C function: {signature}")


class AgentDurabilityContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.io = (AGENT_SOURCE / "io.c").read_text(encoding="utf-8")
        cls.io_header = (AGENT_SOURCE / "io.h").read_text(encoding="utf-8")
        cls.common = (AGENT_SOURCE / "common.h").read_text(encoding="utf-8")
        cls.main = (AGENT_SOURCE / "main.c").read_text(encoding="utf-8")
        cls.result = (AGENT_SOURCE / "result.c").read_text(encoding="utf-8")
        cls.disposable = DISPOSABLE_SOURCE.read_text(encoding="utf-8")

    def test_directory_sync_distinguishes_sync_step_eacces(self) -> None:
        helper = _function(self.io, "static int sync_directory(")

        self.assertIn(
            f"#define VDEV_SCE_ERROR_ERRNO_EACCES UINT32_C({EACCES_HEX})",
            self.io,
        )
        self.assertEqual(helper.count("sceIoDopen(directory)"), 1)
        self.assertEqual(helper.count("sceIoSyncByFd(descriptor, 0)"), 1)
        self.assertEqual(helper.count("sceIoDclose(descriptor)"), 1)
        self.assertEqual(helper.count(DEVICE_SYNC_CALL), 1)
        self.assertLess(helper.index("sceIoDopen"), helper.index("sceIoSyncByFd"))
        self.assertLess(helper.index("sceIoSyncByFd"), helper.index("sceIoDclose"))
        self.assertLess(helper.index("sceIoDclose"), helper.index(DEVICE_SYNC_CALL))
        self.assertIn("if (descriptor < 0) return descriptor;", helper)
        self.assertIn("if (close_result < 0) return close_result;", helper)
        self.assertIn(
            "if ((uint32_t)result == VDEV_SCE_ERROR_ERRNO_EACCES) *sync_eacces = 1;",
            helper,
        )
        self.assertLess(
            helper.index("if (close_result < 0) return close_result;"),
            helper.index("VDEV_SCE_ERROR_ERRNO_EACCES"),
        )

    def test_weak_fallback_is_file_based_and_safe_build_only(self) -> None:
        fallback = _function(self.io, "static int sync_committed_regular_file(")
        parent_sync = _function(self.io, "static int sync_parent_directory(")

        self.assertIn("#if !VDEV_ENABLE_INSTALL", self.io)
        self.assertIn("sceIoGetstat(path, &before)", fallback)
        self.assertIn("sceIoOpen(path, SCE_O_WRONLY, 0)", fallback)
        self.assertIn("sceIoSyncByFd(descriptor, 0)", fallback)
        self.assertIn("sceIoClose(descriptor)", fallback)
        self.assertNotIn("SCE_O_CREAT", fallback)
        self.assertNotIn("SCE_O_TRUNC", fallback)
        self.assertIn(
            "vdev_sha256_file(path, (uint64_t)before.st_size, digest)",
            fallback,
        )
        self.assertLess(
            fallback.index("sceIoSyncByFd"), fallback.index("sceIoClose"),
        )
        self.assertLess(
            fallback.index("sceIoClose"),
            fallback.index("vdev_sha256_file"),
        )
        self.assertIn("#if !VDEV_ENABLE_INSTALL", parent_sync)
        self.assertIn("if (result < 0 && sync_eacces)", parent_sync)
        self.assertIn("return sync_committed_regular_file(path);", parent_sync)
        self.assertIn("#else", parent_sync)

    def test_created_files_grant_user_and_system_read_write_access(self) -> None:
        for flag in (
            "SCE_S_IRUSR",
            "SCE_S_IWUSR",
            "SCE_S_IRSYS",
            "SCE_S_IWSYS",
        ):
            self.assertIn(flag, self.common)
            self.assertIn(flag, self.disposable)
        self.assertIn("SCE_S_IRWXU | SCE_S_IRWXS", self.common)
        self.assertIn("SCE_S_IRWXU | SCE_S_IRWXS", self.disposable)
        self.assertIn(
            "sceIoMkdir(path, VDEV_CREATE_DIRECTORY_MODE)", self.io,
        )
        self.assertIn(
            "sceIoMkdir(TEST_DIRECTORY, TEST_CREATE_DIRECTORY_MODE)",
            self.disposable,
        )

        self.assertRegex(
            self.common,
            r"#define VDEV_CREATE_FILE_MODE\s*\\\s*"
            r"\(SCE_S_IRUSR \| SCE_S_IWUSR \| SCE_S_IRSYS \| SCE_S_IWSYS\)",
        )
        self.assertRegex(
            self.disposable,
            r"#define TEST_CREATE_FILE_MODE\s*\\\s*"
            r"\(SCE_S_IRUSR \| SCE_S_IWUSR \| SCE_S_IRSYS \| SCE_S_IWSYS\)",
        )

        agent_sources = [
            path.read_text(encoding="utf-8") for path in AGENT_SOURCE.glob("*.c")
        ]
        for source in agent_sources:
            for match in re.finditer("SCE_O_CREAT", source):
                self.assertIn(
                    "VDEV_CREATE_FILE_MODE",
                    source[match.end() : match.end() + 180],
                )
        for match in re.finditer("SCE_O_CREAT", self.disposable):
            self.assertIn(
                "TEST_CREATE_FILE_MODE",
                self.disposable[match.end() : match.end() + 180],
            )
        self.assertNotIn("0600", "".join(agent_sources))
        self.assertNotIn("0600", self.disposable)

    def test_device_sync_is_centralized_checked_and_traced(self) -> None:
        sources = list(AGENT_SOURCE.glob("*.c"))
        device_sync_callers = {
            path.name: path.read_text(encoding="utf-8").count("sceIoSync(")
            for path in sources
        }
        self.assertEqual(
            {name: count for name, count in device_sync_callers.items() if count},
            {"io.c": 1},
        )
        helper = _function(self.io, "static int sync_directory(")
        self.assertIn(DEVICE_SYNC_CALL, helper)
        self.assertIn("VDEV_ATOMIC_STEP_DEVICE_SYNC", helper)
        self.assertNotIn('sceIoSync("ux0:", 0)', self.io)
        self.assertNotIn("0x80010016", self.io)
        self.assertIn(
            "int vdev_sync_parent_directory(const char *path);",
            self.io_header,
        )
        self.assertIn(
            "int vdev_sync_rename_parents(const char *source_path,",
            self.io_header,
        )

    def test_atomic_files_and_journals_are_checked_after_rename(self) -> None:
        atomic_write = _function(
            self.io,
            "int vdev_write_atomic(const char *final_path",
        )
        challenge_consume = _function(
            self.io,
            "int vdev_consume_challenge(void)",
        )
        journal_finish = _function(
            self.result,
            "int vdev_journal_finish(VdevJournal *journal)",
        )

        self.assertLess(
            atomic_write.index("sceIoSyncByFd"),
            atomic_write.index("sceIoRename"),
        )
        self.assertLess(
            atomic_write.index("sceIoRename"),
            atomic_write.index("sceIoGetstat(final_path"),
        )
        self.assertLess(
            atomic_write.index("sceIoGetstat(final_path"),
            atomic_write.index("sync_parent_directory"),
        )
        self.assertLess(
            challenge_consume.index("sceIoRename"),
            challenge_consume.index("vdev_is_regular_file(VDEV_CHALLENGE_USED)"),
        )
        self.assertLess(
            challenge_consume.index("vdev_is_regular_file(VDEV_CHALLENGE_USED)"),
            challenge_consume.index("vdev_sync_rename_parents"),
        )
        self.assertLess(
            journal_finish.index("sceIoRename"),
            journal_finish.index("vdev_is_regular_file(journal->final_path)"),
        )
        self.assertLess(
            journal_finish.index("vdev_is_regular_file(journal->final_path)"),
            journal_finish.index("vdev_sync_rename_parents"),
        )

    def test_promotion_paths_use_strict_parent_directory_syncs(self) -> None:
        clear_state = _function(self.main, "static int clear_promotion_state(void)")

        self.assertIn("sceIoRemove(VDEV_PROMOTE_STATE)", clear_state)
        self.assertIn("vdev_is_regular_file(VDEV_PROMOTE_STATE)", clear_state)
        self.assertIn(
            "return vdev_sync_parent_directory(VDEV_PROMOTE_STATE);",
            clear_state,
        )
        self.assertIn(
            "sceIoRename(job_package_directory, VDEV_PROMOTE_ROOT)",
            self.main,
        )
        self.assertIn("sceIoRename(VDEV_PROMOTE_ROOT,", self.main)
        self.assertIn("!vdev_is_directory(VDEV_PROMOTE_ROOT)", self.main)
        self.assertIn("!vdev_is_directory(job_package_directory)", self.main)
        self.assertEqual(self.main.count("vdev_sync_rename_parents("), 2)

    def test_cross_directory_rename_never_uses_weak_fallback(self) -> None:
        helper = _function(self.io, "int vdev_sync_rename_parents(")
        cross_directory = helper[helper.index("Cross-directory promotion") :]

        self.assertEqual(helper.count("parent_directory_for_path("), 2)
        self.assertEqual(helper.count("sync_directory("), 3)
        self.assertIn(
            "if (strcmp(source_parent, destination_parent) == 0)",
            helper,
        )
        self.assertIn("#if !VDEV_ENABLE_INSTALL", helper)
        self.assertIn("sync_committed_regular_file(destination_path)", helper)
        self.assertNotIn("sync_committed_regular_file", cross_directory)
        self.assertNotIn("if (result < 0 && sync_eacces)", cross_directory)
        self.assertLess(
            cross_directory.index("sync_directory(destination_parent"),
            cross_directory.index("sync_directory(source_parent"),
        )

    def test_disposable_marker_uses_file_based_weak_fallback(self) -> None:
        helper = _function(self.disposable, "static int sync_test_directory(")
        fallback = _function(self.disposable, "static int weak_sync_test_marker(")
        marker = _function(
            self.disposable,
            "static int write_launch_marker(uint64_t launch_count)",
        )

        self.assertIn(
            f"#define TEST_SCE_ERROR_ERRNO_EACCES UINT32_C({EACCES_HEX})",
            self.disposable,
        )
        self.assertNotIn("sceIoSync(", self.disposable)
        self.assertIn("sceIoDopen(TEST_DIRECTORY)", helper)
        self.assertIn("sceIoSyncByFd(descriptor, 0)", helper)
        self.assertIn("sceIoDclose(descriptor)", helper)
        self.assertIn("if (close_result < 0) return close_result;", helper)
        self.assertIn(
            "(uint32_t)result == TEST_SCE_ERROR_ERRNO_EACCES",
            helper,
        )
        self.assertIn("return weak_sync_test_marker(expected_size);", helper)
        self.assertLess(
            helper.index("if (close_result < 0) return close_result;"),
            helper.index("TEST_SCE_ERROR_ERRNO_EACCES"),
        )
        self.assertIn("sceIoOpen(TEST_MARKER, SCE_O_WRONLY, 0)", fallback)
        self.assertIn("sceIoOpen(TEST_MARKER, SCE_O_RDONLY, 0)", fallback)
        self.assertIn("sceIoSyncByFd(descriptor, 0)", fallback)
        self.assertNotIn("SCE_O_CREAT", fallback)
        self.assertNotIn("SCE_O_TRUNC", fallback)
        self.assertIn("marker + offset", fallback)
        self.assertIn("sceIoRead(descriptor, &extra, 1)", fallback)
        self.assertIn("sceIoClose(descriptor)", fallback)
        self.assertIn("sceIoGetstat(TEST_MARKER, &status)", fallback)
        self.assertIn("while (result >= 0 && offset < expected_size)", fallback)
        self.assertLess(
            fallback.index("sceIoOpen(TEST_MARKER, SCE_O_WRONLY, 0)"),
            fallback.index("sceIoSyncByFd(descriptor, 0)"),
        )
        self.assertLess(
            fallback.index("sceIoSyncByFd(descriptor, 0)"),
            fallback.index("sceIoOpen(TEST_MARKER, SCE_O_RDONLY, 0)"),
        )
        close_positions = [
            match.start() for match in re.finditer(r"sceIoClose\(descriptor\)", fallback)
        ]
        self.assertEqual(len(close_positions), 2)
        write_open = fallback.index("sceIoOpen(TEST_MARKER, SCE_O_WRONLY, 0)")
        sync = fallback.index("sceIoSyncByFd(descriptor, 0)")
        read_open = fallback.index("sceIoOpen(TEST_MARKER, SCE_O_RDONLY, 0)")
        read = fallback.index("sceIoRead(")
        poststat = fallback.index("sceIoGetstat(TEST_MARKER, &status)")
        self.assertLess(write_open, sync)
        self.assertLess(sync, close_positions[0])
        self.assertLess(close_positions[0], read_open)
        self.assertLess(read_open, read)
        self.assertLess(read, close_positions[1])
        self.assertLess(close_positions[1], poststat)
        self.assertLess(marker.index("sceIoSyncByFd"), marker.index("sceIoRename"))
        self.assertLess(
            marker.index("sceIoRename"), marker.rindex("sceIoGetstat(TEST_MARKER"),
        )
        self.assertLess(
            marker.rindex("sceIoGetstat(TEST_MARKER"),
            marker.index("sync_test_directory(length)"),
        )


if __name__ == "__main__":
    unittest.main()
