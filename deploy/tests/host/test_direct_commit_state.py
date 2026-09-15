from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
HEADER_ROOT = PROJECT_ROOT / "agent" / "src"


class DirectCommitStateNativeTests(unittest.TestCase):
    def test_fake_io_failure_matrix(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            bundled = Path("C:/msys64/mingw64/bin/gcc.exe")
            if bundled.is_file():
                compiler = str(bundled)
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        source = r'''
#include "direct_commit_state.h"

typedef struct FakeIo {
    int metadata_result;
    int phase1_ack_result;
    int request_rename_result;
    int post_commit_result;
    int cleanup_result;
} FakeIo;

static VdevDirectCommitState fake_handler(FakeIo io, int *cleanup_called)
{
    VdevDirectTransaction transaction;
    vdev_direct_transaction_init(&transaction);
    *cleanup_called = 0;
    vdev_direct_transaction_take_staging(&transaction);

    if (io.metadata_result < 0 || io.phase1_ack_result < 0) {
        *cleanup_called = vdev_direct_transaction_needs_cleanup(&transaction);
        return vdev_direct_transaction_failure(&transaction,
                                                io.cleanup_result);
    }
    if (io.request_rename_result >= 0) {
        vdev_direct_transaction_record_request_rename(&transaction);
    }
    if (io.request_rename_result < 0 || io.post_commit_result < 0) {
        *cleanup_called = vdev_direct_transaction_needs_cleanup(&transaction);
        return vdev_direct_transaction_failure(&transaction,
            *cleanup_called ? io.cleanup_result : 0);
    }
    return VDEV_DIRECT_KNOWN_COMMITTED;
}

int main(void)
{
    int cleanup_called;
    FakeIo io = {0, 0, 0, 0, 0};

    if (vdev_direct_classify_path_probe(0, 1, 0) !=
        VDEV_DIRECT_PATH_MISSING) return 1;
    if (vdev_direct_classify_path_probe(0, 0, 0) !=
        VDEV_DIRECT_PATH_IO_ERROR) return 2;
    if (vdev_direct_classify_path_probe(1, 0, 1) !=
        VDEV_DIRECT_PATH_DIRECTORY) return 3;
    if (vdev_direct_classify_path_probe(1, 0, 0) !=
        VDEV_DIRECT_PATH_OTHER) return 4;

    io.metadata_result = -1;
    if (fake_handler(io, &cleanup_called) != VDEV_DIRECT_FAIL_BEFORE_COMMIT ||
        cleanup_called != 1) return 5;
    io.cleanup_result = -1;
    if (fake_handler(io, &cleanup_called) !=
        VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT || cleanup_called != 1) return 6;

    io.metadata_result = 0;
    io.phase1_ack_result = -1;
    io.cleanup_result = 0;
    if (fake_handler(io, &cleanup_called) != VDEV_DIRECT_FAIL_BEFORE_COMMIT ||
        cleanup_called != 1) return 7;
    io.cleanup_result = -1;
    if (fake_handler(io, &cleanup_called) !=
        VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT || cleanup_called != 1) return 8;

    io.phase1_ack_result = 0;
    io.request_rename_result = -1;
    io.cleanup_result = 0;
    if (fake_handler(io, &cleanup_called) != VDEV_DIRECT_FAIL_BEFORE_COMMIT ||
        cleanup_called != 1) return 9;
    io.cleanup_result = -1;
    if (fake_handler(io, &cleanup_called) !=
        VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT || cleanup_called != 1) return 10;

    io.request_rename_result = 0;
    io.post_commit_result = -1;
    io.cleanup_result = 0;
    if (fake_handler(io, &cleanup_called) !=
        VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT || cleanup_called != 0) return 11;
    io.post_commit_result = 0;
    if (fake_handler(io, &cleanup_called) != VDEV_DIRECT_KNOWN_COMMITTED ||
        cleanup_called != 0) return 12;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_path = root / "test.c"
            executable = root / ("test.exe" if os.name == "nt" else "test")
            source_path.write_text(source, encoding="ascii")
            environment = os.environ.copy()
            compiler_directory = str(Path(compiler).resolve().parent)
            environment["PATH"] = compiler_directory + os.pathsep + environment.get("PATH", "")
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(HEADER_ROOT),
                    str(source_path),
                    "-o",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
                env=environment,
                timeout=30,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], check=False, capture_output=True, text=True
            )
            self.assertEqual(run_result.returncode, 0, run_result.stderr)


if __name__ == "__main__":
    unittest.main()
