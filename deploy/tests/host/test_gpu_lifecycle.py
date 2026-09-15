from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host"))

from vitadevdeploy.gpu_lifecycle import (
    GpuLifecycleError,
    GpuLifecycleModel,
    run_lifecycle_model,
)


class GpuLifecycleModelTests(unittest.TestCase):
    def test_large_deterministic_schedule_balances_every_present_with_wait(self) -> None:
        result = run_lifecycle_model(cycles=10000, frames_per_cycle=8)
        self.assertEqual(result["frames_presented"], 80000)
        self.assertEqual(result["render_waits"], 80000)
        self.assertEqual(result["violations"], 0)

    def test_transient_pool_reuse_without_wait_is_rejected(self) -> None:
        model = GpuLifecycleModel()
        model.initialize()
        model.begin_frame()
        model.present()
        with self.assertRaisesRegex(GpuLifecycleError, "transient-pool"):
            model.begin_frame()

    def test_finish_with_in_flight_frame_is_rejected(self) -> None:
        model = GpuLifecycleModel()
        model.initialize()
        model.begin_frame()
        model.present()
        with self.assertRaisesRegex(GpuLifecycleError, "in flight"):
            model.finish()

    def test_orderly_finish_accepts_one_final_wait(self) -> None:
        model = GpuLifecycleModel()
        model.initialize()
        model.begin_frame()
        model.present()
        model.wait_rendering_done()
        model.finish()
        self.assertTrue(model.finished)
        self.assertEqual(model.waits, 1)

    def test_unbounded_parameters_are_rejected(self) -> None:
        for cycles, frames in ((0, 1), (100001, 1), (1, 0), (1, 10001)):
            with self.subTest(cycles=cycles, frames=frames):
                with self.assertRaises(ValueError):
                    run_lifecycle_model(cycles=cycles, frames_per_cycle=frames)


if __name__ == "__main__":
    unittest.main()
