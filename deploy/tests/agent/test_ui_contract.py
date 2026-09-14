from __future__ import annotations

import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
AGENT_SOURCE = PROJECT_ROOT / "agent" / "src"


class AgentUiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ui = (AGENT_SOURCE / "ui.c").read_text(encoding="utf-8")
        cls.header = (AGENT_SOURCE / "ui.h").read_text(encoding="utf-8")
        cls.main = (AGENT_SOURCE / "main.c").read_text(encoding="utf-8")
        cls.cmake = (PROJECT_ROOT / "agent" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

    def test_display_ui_is_graphical_vita2d_not_character_cells(self) -> None:
        self.assertIn("#include <vita2d.h>", self.ui)
        self.assertIn("vita2d_draw_rectangle", self.ui)
        self.assertIn("vita2d_draw_fill_circle", self.ui)
        self.assertIn("vita2d_pgf_draw_text", self.ui)
        self.assertNotIn("debugScreen", self.ui)

    def test_every_graphical_frame_has_a_complete_present_cycle(self) -> None:
        frame = self.ui[
            self.ui.index("static void draw_frame(") :
            self.ui.index("int vdev_ui_init(void)")
        ]
        self.assertEqual(frame.count("vita2d_start_drawing();"), 1)
        self.assertEqual(frame.count("vita2d_end_drawing();"), 1)
        self.assertEqual(frame.count("vita2d_swap_buffers();"), 1)
        self.assertLess(
            frame.index("vita2d_start_drawing();"),
            frame.index("vita2d_end_drawing();"),
        )
        self.assertLess(
            frame.index("vita2d_end_drawing();"),
            frame.index("vita2d_swap_buffers();"),
        )

    def test_normal_exit_releases_font_and_vita2d(self) -> None:
        self.assertIn("vita2d_wait_rendering_done();", self.ui)
        self.assertIn("vita2d_free_pgf(ui_font);", self.ui)
        self.assertIn("vita2d_fini();", self.ui)
        self.assertIn("vdev_ui_finish();", self.main)

    def test_waiting_animation_is_driven_by_existing_bounded_poll(self) -> None:
        self.assertIn("void vdev_ui_waiting_tick(void);", self.header)
        delay = "sceKernelDelayThread(250u * 1000u);"
        tick = "vdev_ui_waiting_tick();"
        self.assertIn(delay, self.main)
        self.assertIn(tick, self.main)
        self.assertLess(self.main.index(delay), self.main.index(tick))
        self.assertIn('"Waiting for connection"', self.ui)

    def test_ui_exposes_determinate_and_indeterminate_progress(self) -> None:
        self.assertIn("static void draw_progress_bar(", self.ui)
        self.assertIn("if (indeterminate)", self.ui)
        self.assertIn("(float)percent / 100.0f", self.ui)
        self.assertIn("VDEV_UI_MODE_INSTALLING", self.ui)
        self.assertIn("VDEV_UI_MODE_COMPLETE", self.ui)
        self.assertIn("VDEV_UI_MODE_ERROR", self.ui)

    def test_graphics_dependency_is_ui_only(self) -> None:
        self.assertIn("if(VDEV_ENABLE_DISPLAY_UI)", self.cmake)
        self.assertIn("find_library(VITA2D_LIBRARY vita2d REQUIRED)", self.cmake)
        self.assertIn("${VITA2D_LIBRARY}", self.cmake)


if __name__ == "__main__":
    unittest.main()
