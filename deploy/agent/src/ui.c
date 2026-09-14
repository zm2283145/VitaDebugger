#include "ui.h"

#include "common.h"
#include "promoter.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if VDEV_ENABLE_DISPLAY_UI
#include <vita2d.h>

#define VDEV_UI_PROGRESS_X 86.0f
#define VDEV_UI_PROGRESS_Y 303.0f
#define VDEV_UI_PROGRESS_WIDTH 788.0f
#define VDEV_UI_PROGRESS_HEIGHT 18.0f
#define VDEV_UI_ACTIVITY_WIDTH 156.0f
#define VDEV_UI_TEXT_LIMIT 104u

enum VdevUiMode {
    VDEV_UI_MODE_WORKING = 0,
    VDEV_UI_MODE_WAITING = 1,
    VDEV_UI_MODE_INSTALLING = 2,
    VDEV_UI_MODE_COMPLETE = 3,
    VDEV_UI_MODE_ERROR = 4
};

static const unsigned int COLOR_BACKGROUND = RGBA8(10, 18, 32, 255);
static const unsigned int COLOR_BACKGROUND_GLOW = RGBA8(16, 46, 66, 255);
static const unsigned int COLOR_PANEL = RGBA8(20, 31, 51, 255);
static const unsigned int COLOR_PANEL_EDGE = RGBA8(43, 59, 82, 255);
static const unsigned int COLOR_PANEL_SOFT = RGBA8(27, 41, 63, 255);
static const unsigned int COLOR_TRACK = RGBA8(38, 52, 73, 255);
static const unsigned int COLOR_ACCENT = RGBA8(34, 211, 238, 255);
static const unsigned int COLOR_ACCENT_DARK = RGBA8(8, 93, 112, 255);
static const unsigned int COLOR_TEXT = RGBA8(241, 245, 249, 255);
static const unsigned int COLOR_TEXT_MUTED = RGBA8(148, 163, 184, 255);
static const unsigned int COLOR_TEXT_DIM = RGBA8(100, 116, 139, 255);
static const unsigned int COLOR_SUCCESS = RGBA8(74, 222, 128, 255);
static const unsigned int COLOR_SUCCESS_DARK = RGBA8(22, 101, 52, 255);
static const unsigned int COLOR_ERROR = RGBA8(251, 113, 133, 255);
static const unsigned int COLOR_ERROR_DARK = RGBA8(127, 29, 29, 255);

static int ui_ready;
static vita2d_pgf *ui_font;
static uint64_t waiting_frame;
static uint64_t last_promotion_frame = UINT64_MAX;

static void draw_rounded_rectangle(float x, float y, float width, float height,
                                   float radius, unsigned int color)
{
    if (width <= 0.0f || height <= 0.0f) return;
    if (radius < 0.0f) radius = 0.0f;
    if (radius * 2.0f > width) radius = width * 0.5f;
    if (radius * 2.0f > height) radius = height * 0.5f;

    vita2d_draw_rectangle(x + radius, y, width - radius * 2.0f,
                          height, color);
    vita2d_draw_rectangle(x, y + radius, width,
                          height - radius * 2.0f, color);
    vita2d_draw_fill_circle(x + radius, y + radius, radius, color);
    vita2d_draw_fill_circle(x + width - radius, y + radius, radius, color);
    vita2d_draw_fill_circle(x + radius, y + height - radius, radius, color);
    vita2d_draw_fill_circle(x + width - radius, y + height - radius,
                            radius, color);
}

static void copy_display_text(char output[VDEV_UI_TEXT_LIMIT + 1u],
                              const char *input)
{
    size_t length;

    if (input == NULL || input[0] == '\0') input = "Please wait";
    length = strlen(input);
    if (length <= VDEV_UI_TEXT_LIMIT) {
        memcpy(output, input, length + 1u);
        return;
    }
    memcpy(output, input, VDEV_UI_TEXT_LIMIT - 3u);
    memcpy(output + VDEV_UI_TEXT_LIMIT - 3u, "...", 4u);
}

static void draw_text(int x, int y, unsigned int color, float scale,
                      const char *text)
{
    char bounded[VDEV_UI_TEXT_LIMIT + 1u];
    if (ui_font == NULL) return;
    copy_display_text(bounded, text);
    vita2d_pgf_draw_text(ui_font, x, y, color, scale, bounded);
}

static void draw_brand_mark(void)
{
    draw_rounded_rectangle(48.0f, 30.0f, 54.0f, 54.0f, 14.0f,
                           COLOR_ACCENT_DARK);
    vita2d_draw_rectangle(73.0f, 42.0f, 5.0f, 22.0f, COLOR_ACCENT);
    vita2d_draw_line(64.0f, 57.0f, 75.5f, 69.0f, COLOR_ACCENT);
    vita2d_draw_line(87.0f, 57.0f, 75.5f, 69.0f, COLOR_ACCENT);
    vita2d_draw_rectangle(62.0f, 72.0f, 27.0f, 3.0f, COLOR_ACCENT);
}

static void draw_header(void)
{
    draw_brand_mark();
    draw_text(122, 55, COLOR_TEXT, 1.25f, "VitaDevDeploy");
    draw_text(122, 78, COLOR_TEXT_MUTED, 0.78f,
              "Secure deployment agent");

#if VDEV_ENABLE_INSTALL
    draw_rounded_rectangle(741.0f, 42.0f, 171.0f, 32.0f, 16.0f,
                           COLOR_SUCCESS_DARK);
    draw_text(766, 65, COLOR_SUCCESS, 0.72f, "INSTALL ENABLED");
#else
    draw_rounded_rectangle(751.0f, 42.0f, 161.0f, 32.0f, 16.0f,
                           COLOR_ACCENT_DARK);
    draw_text(777, 65, COLOR_ACCENT, 0.72f, "VERIFY ONLY");
#endif
}

static void draw_status_icon(enum VdevUiMode mode, uint64_t animation_frame)
{
    unsigned int color = COLOR_ACCENT;
    unsigned int dark = COLOR_ACCENT_DARK;

    if (mode == VDEV_UI_MODE_COMPLETE) {
        color = COLOR_SUCCESS;
        dark = COLOR_SUCCESS_DARK;
    } else if (mode == VDEV_UI_MODE_ERROR) {
        color = COLOR_ERROR;
        dark = COLOR_ERROR_DARK;
    }

    vita2d_draw_fill_circle(104.0f, 163.0f, 25.0f, dark);
    if (mode == VDEV_UI_MODE_COMPLETE) {
        vita2d_draw_line(92.0f, 163.0f, 101.0f, 172.0f, color);
        vita2d_draw_line(101.0f, 172.0f, 118.0f, 151.0f, color);
    } else if (mode == VDEV_UI_MODE_ERROR) {
        vita2d_draw_rectangle(101.5f, 148.0f, 5.0f, 19.0f, color);
        vita2d_draw_fill_circle(104.0f, 176.0f, 3.0f, color);
    } else if (mode == VDEV_UI_MODE_WAITING ||
               mode == VDEV_UI_MODE_INSTALLING) {
        int index;
        for (index = 0; index < 8; ++index) {
            static const int x_offsets[8] = {0, 8, 11, 8, 0, -8, -11, -8};
            static const int y_offsets[8] = {-11, -8, 0, 8, 11, 8, 0, -8};
            const uint64_t distance =
                ((uint64_t)index + 8u - (animation_frame % 8u)) % 8u;
            const float radius = distance == 0u ? 3.5f : 2.0f;
            const unsigned int dot_color = distance < 3u ? color : COLOR_TEXT_DIM;
            vita2d_draw_fill_circle(104.0f + (float)x_offsets[index],
                                    163.0f + (float)y_offsets[index],
                                    radius, dot_color);
        }
    } else {
        vita2d_draw_rectangle(101.5f, 149.0f, 5.0f, 18.0f, color);
        vita2d_draw_fill_circle(104.0f, 175.0f, 3.0f, color);
    }
}

static void draw_progress_bar(int percent, int indeterminate,
                              uint64_t animation_frame,
                              unsigned int active_color)
{
    draw_rounded_rectangle(VDEV_UI_PROGRESS_X, VDEV_UI_PROGRESS_Y,
                           VDEV_UI_PROGRESS_WIDTH, VDEV_UI_PROGRESS_HEIGHT,
                           9.0f, COLOR_TRACK);

    if (indeterminate) {
        const float travel = VDEV_UI_PROGRESS_WIDTH - VDEV_UI_ACTIVITY_WIDTH;
        const float position =
            (float)((animation_frame * 29u) % (uint64_t)(travel + 1.0f));
        draw_rounded_rectangle(VDEV_UI_PROGRESS_X + position,
                               VDEV_UI_PROGRESS_Y,
                               VDEV_UI_ACTIVITY_WIDTH,
                               VDEV_UI_PROGRESS_HEIGHT, 9.0f, active_color);
        return;
    }

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent > 0) {
        float width = VDEV_UI_PROGRESS_WIDTH * (float)percent / 100.0f;
        if (width < VDEV_UI_PROGRESS_HEIGHT) width = VDEV_UI_PROGRESS_HEIGHT;
        draw_rounded_rectangle(VDEV_UI_PROGRESS_X, VDEV_UI_PROGRESS_Y,
                               width, VDEV_UI_PROGRESS_HEIGHT, 9.0f,
                               active_color);
    }
}

static void draw_milestones(int percent, unsigned int active_color)
{
    static const int milestone_percent[] = {10, 28, 44, 65, 82, 100};
    static const char *milestone_label[] = {
        "Connect", "Request", "Authenticate", "Verify", "Install", "Done"
    };
    int index;

    for (index = 0; index < 6; ++index) {
        const float x = 92.0f + (float)index * 153.5f;
        const int reached = percent >= milestone_percent[index];
        vita2d_draw_fill_circle(x, 358.0f, reached ? 6.0f : 4.0f,
                                reached ? active_color : COLOR_TEXT_DIM);
        draw_text((int)x - 23, 387,
                  reached ? COLOR_TEXT_MUTED : COLOR_TEXT_DIM,
                  0.58f, milestone_label[index]);
    }
}

static const char *mode_label(enum VdevUiMode mode)
{
    switch (mode) {
        case VDEV_UI_MODE_WAITING: return "READY";
        case VDEV_UI_MODE_INSTALLING: return "INSTALLING";
        case VDEV_UI_MODE_COMPLETE: return "COMPLETE";
        case VDEV_UI_MODE_ERROR: return "ATTENTION";
        default: return "IN PROGRESS";
    }
}

static void draw_frame(enum VdevUiMode mode, int percent,
                       const char *stage, const char *detail,
                       const char *metadata, uint64_t animation_frame)
{
    unsigned int active_color = COLOR_ACCENT;
    unsigned int chip_color = COLOR_ACCENT_DARK;
    const int indeterminate =
        mode == VDEV_UI_MODE_WAITING || mode == VDEV_UI_MODE_INSTALLING;

    if (mode == VDEV_UI_MODE_COMPLETE) {
        active_color = COLOR_SUCCESS;
        chip_color = COLOR_SUCCESS_DARK;
    } else if (mode == VDEV_UI_MODE_ERROR) {
        active_color = COLOR_ERROR;
        chip_color = COLOR_ERROR_DARK;
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_fill_circle(872.0f, -20.0f, 205.0f,
                            COLOR_BACKGROUND_GLOW);
    vita2d_draw_fill_circle(26.0f, 558.0f, 165.0f,
                            RGBA8(13, 52, 70, 255));
    draw_header();

    draw_rounded_rectangle(47.0f, 117.0f, 866.0f, 324.0f, 22.0f,
                           COLOR_PANEL_EDGE);
    draw_rounded_rectangle(49.0f, 119.0f, 862.0f, 320.0f, 20.0f,
                           COLOR_PANEL);
    draw_status_icon(mode, animation_frame);

    draw_rounded_rectangle(146.0f, 139.0f,
                           mode == VDEV_UI_MODE_WORKING ? 126.0f : 112.0f,
                           28.0f, 14.0f, chip_color);
    draw_text(161, 160, active_color, 0.64f, mode_label(mode));
    draw_text(146, 205, COLOR_TEXT, 1.18f,
              stage != NULL ? stage : "Working");
    draw_text(146, 238, COLOR_TEXT_MUTED, 0.76f,
              detail != NULL ? detail : "Please wait");
    if (metadata != NULL && metadata[0] != '\0') {
        draw_text(146, 267, COLOR_TEXT_DIM, 0.66f, metadata);
    }

    draw_progress_bar(percent, indeterminate, animation_frame, active_color);
    if (!indeterminate) {
        char percent_text[16];
        const int bounded_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
        snprintf(percent_text, sizeof(percent_text), "%d%%", bounded_percent);
        draw_rounded_rectangle(814.0f, 258.0f, 60.0f, 30.0f, 15.0f,
                               COLOR_PANEL_SOFT);
        draw_text(829, 280, active_color, 0.68f, percent_text);
    }
    draw_milestones(percent, active_color);

    draw_rounded_rectangle(48.0f, 465.0f, 864.0f, 48.0f, 14.0f,
                           COLOR_PANEL_SOFT);
    vita2d_draw_fill_circle(73.0f, 489.0f, 7.0f, active_color);
    draw_text(91, 495, COLOR_TEXT_MUTED, 0.66f,
              mode == VDEV_UI_MODE_WAITING
                  ? "Waiting securely - press Circle to return to LiveArea"
                  : mode == VDEV_UI_MODE_ERROR
                      ? "The host result contains the full failure details"
                      : "Keep Wi-Fi connected and do not power off during installation");

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int vdev_ui_init(void)
{
    int result = vita2d_init();
    if (result < 0) return result;

    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(COLOR_BACKGROUND);
    ui_font = vita2d_load_default_pgf();
    if (ui_font == NULL) {
        vita2d_fini();
        return VDEV_ERR_INTERNAL;
    }

    ui_ready = 1;
    waiting_frame = 0u;
    last_promotion_frame = UINT64_MAX;
    vdev_ui_status(2, "Starting", "Initializing the deployment agent");
    return 0;
}

void vdev_ui_finish(void)
{
    if (!ui_ready) return;
    vita2d_wait_rendering_done();
    vita2d_free_pgf(ui_font);
    ui_font = NULL;
    vita2d_fini();
    ui_ready = 0;
}

void vdev_ui_status(int percent, const char *stage, const char *detail)
{
    if (!ui_ready) return;
    draw_frame(VDEV_UI_MODE_WORKING, percent, stage, detail, NULL, 0u);
}

void vdev_ui_waiting(void)
{
    if (!ui_ready) return;
    waiting_frame = 0u;
    draw_frame(VDEV_UI_MODE_WAITING, 10, "Waiting for connection",
               "Ready for a signed deployment from your development PC",
               "No package is being installed", waiting_frame);
}

void vdev_ui_waiting_tick(void)
{
    if (!ui_ready) return;
    ++waiting_frame;
    draw_frame(VDEV_UI_MODE_WAITING, 10, "Waiting for connection",
               "Ready for a signed deployment from your development PC",
               "No package is being installed", waiting_frame);
}

void vdev_ui_promotion_progress(int state, uint64_t elapsed_milliseconds,
                                void *context)
{
    const uint64_t frame = elapsed_milliseconds / UINT64_C(250);
    char metadata[96];
    const char *stage = "Installing package";
    const char *detail = "The Vita system installer is applying the package";
    (void)context;

    if (!ui_ready || frame == last_promotion_frame) return;
    last_promotion_frame = frame;

    if (state == VDEV_PROMOTE_PROGRESS_STATE_UNAVAILABLE) {
        detail = "Installer status is temporarily unavailable; waiting safely";
        snprintf(metadata, sizeof(metadata), "Waiting for status  -  %llu seconds elapsed",
                 (unsigned long long)(elapsed_milliseconds / 1000u));
    } else if (state == VDEV_PROMOTE_PROGRESS_RESULT_UNAVAILABLE) {
        stage = "Finalizing installation";
        detail = "Package transfer finished; waiting for the final result";
        snprintf(metadata, sizeof(metadata), "Finalizing  -  %llu seconds elapsed",
                 (unsigned long long)(elapsed_milliseconds / 1000u));
    } else {
        snprintf(metadata, sizeof(metadata),
                 "Installer state %d  -  %llu seconds elapsed", state,
                 (unsigned long long)(elapsed_milliseconds / 1000u));
    }
    draw_frame(VDEV_UI_MODE_INSTALLING, 86, stage, detail, metadata, frame);
}

void vdev_ui_complete(const char *title_id, const char *detail)
{
    char metadata[64];
    if (!ui_ready) return;
    snprintf(metadata, sizeof(metadata), "Title ID: %s",
             title_id != NULL ? title_id : "unknown");
    draw_frame(VDEV_UI_MODE_COMPLETE, 100, "Deployment complete",
               detail != NULL ? detail : "Success", metadata, 0u);
}

void vdev_ui_error(const char *stage, int code, const char *detail)
{
    char title[96];
    char metadata[64];
    if (!ui_ready) return;
    snprintf(title, sizeof(title), "Stopped during %s",
             stage != NULL ? stage : "an unknown stage");
    snprintf(metadata, sizeof(metadata), "Error %d  (0x%08X)",
             code, (unsigned int)code);
    draw_frame(VDEV_UI_MODE_ERROR, 0, title,
               detail != NULL ? detail : "Unknown failure", metadata, 0u);
}

#else

/* The production default remains headless; all calls compile to no-ops. */
int vdev_ui_init(void)
{
    return 0;
}

void vdev_ui_finish(void)
{
}

void vdev_ui_status(int percent, const char *stage, const char *detail)
{
    (void)percent;
    (void)stage;
    (void)detail;
}

void vdev_ui_waiting(void)
{
}

void vdev_ui_waiting_tick(void)
{
}

void vdev_ui_promotion_progress(int state, uint64_t elapsed_milliseconds,
                                void *context)
{
    (void)state;
    (void)elapsed_milliseconds;
    (void)context;
}

void vdev_ui_complete(const char *title_id, const char *detail)
{
    (void)title_id;
    (void)detail;
}

void vdev_ui_error(const char *stage, int code, const char *detail)
{
    (void)stage;
    (void)code;
    (void)detail;
}

#endif
