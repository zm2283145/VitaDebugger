"""Deterministic model for VitaDevDeploy's optional display lifecycle.

This is deliberately not a claim about SceGxm hardware.  It makes the host
stress plan reproducible and rejects lifecycle schedules that reuse the shared
vita2d transient pool or finalize it while a prior frame can still be in
flight.  Hardware launch/peel/exit stress remains a separate gate.
"""

from __future__ import annotations

from dataclasses import dataclass


class GpuLifecycleError(RuntimeError):
    """A modeled display operation violated the ownership contract."""


@dataclass
class GpuLifecycleModel:
    initialized: bool = False
    frame_open: bool = False
    frame_in_flight: bool = False
    finished: bool = False
    frames_presented: int = 0
    waits: int = 0

    def initialize(self) -> None:
        if self.initialized or self.finished:
            raise GpuLifecycleError("display may be initialized exactly once per process")
        self.initialized = True

    def begin_frame(self) -> None:
        if not self.initialized or self.finished or self.frame_open:
            raise GpuLifecycleError("frame begin is outside an initialized idle display")
        if self.frame_in_flight:
            raise GpuLifecycleError("prior frame must finish before transient-pool reuse")
        self.frame_open = True

    def present(self) -> None:
        if not self.frame_open or self.finished:
            raise GpuLifecycleError("only an open frame can be presented")
        self.frame_open = False
        self.frame_in_flight = True
        self.frames_presented += 1

    def wait_rendering_done(self) -> None:
        if not self.initialized or self.finished or self.frame_open:
            raise GpuLifecycleError("render wait is outside an initialized frame boundary")
        if self.frame_in_flight:
            self.frame_in_flight = False
            self.waits += 1

    def finish(self) -> None:
        if not self.initialized or self.finished or self.frame_open:
            raise GpuLifecycleError("display finish is outside an initialized frame boundary")
        if self.frame_in_flight:
            raise GpuLifecycleError("display cannot finalize while a frame is in flight")
        self.finished = True


def run_lifecycle_model(*, cycles: int, frames_per_cycle: int = 8) -> dict[str, int]:
    if not 1 <= cycles <= 100_000:
        raise ValueError("cycles must be between 1 and 100000")
    if not 1 <= frames_per_cycle <= 10_000:
        raise ValueError("frames per cycle must be between 1 and 10000")
    total_frames = 0
    total_waits = 0
    for _ in range(cycles):
        model = GpuLifecycleModel()
        model.initialize()
        for frame in range(frames_per_cycle):
            if frame:
                model.wait_rendering_done()
            model.begin_frame()
            model.present()
        model.wait_rendering_done()
        model.finish()
        total_frames += model.frames_presented
        total_waits += model.waits
    return {
        "cycles": cycles,
        "frames_per_cycle": frames_per_cycle,
        "frames_presented": total_frames,
        "render_waits": total_waits,
        "violations": 0,
    }


__all__ = ["GpuLifecycleError", "GpuLifecycleModel", "run_lifecycle_model"]
