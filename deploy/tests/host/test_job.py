from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.errors import ProtocolError
from vitadevdeploy.job import prepare_job

try:
    from .helpers import make_head_template, make_vpk
except ImportError:
    from helpers import make_head_template, make_vpk


class JobTests(unittest.TestCase):
    def test_default_installer_rejects_self_update(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vpk = make_vpk(root / "agent.vpk", title_id="VDEVDEP01")
            template = root / "head.bin"
            template.write_bytes(make_head_template())
            with patch("vitadevdeploy.job.sign", return_value=b"s" * 64):
                with self.assertRaises(ProtocolError):
                    prepare_job(
                        vpk_path=vpk,
                        output_directory=root / "jobs",
                        nonce="ab" * 32,
                        private_key=root / "unused.pem",
                        head_template=template,
                    )

    def test_bootstrap_identity_can_prepare_real_installer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vpk = make_vpk(root / "agent.vpk", title_id="VDEVDEP01")
            template = root / "head.bin"
            template.write_bytes(make_head_template())
            with patch("vitadevdeploy.job.sign", return_value=b"s" * 64):
                job = prepare_job(
                    vpk_path=vpk,
                    output_directory=root / "jobs",
                    nonce="ab" * 32,
                    private_key=root / "unused.pem",
                    head_template=template,
                    installer_title_id="SLRS00001",
                    action="verify",
                    job_id="12" * 16,
                )
            self.assertEqual(job.request.title_id, "VDEVDEP01")
            self.assertIn("sce_sys/package/head.bin", {entry.path for entry in job.manifest.entries})
            self.assertEqual(job.signature_path.read_bytes(), b"s" * 64)


if __name__ == "__main__":
    unittest.main()
