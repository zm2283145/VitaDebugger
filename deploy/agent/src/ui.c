#include "ui.h"

#include "common.h"
#include "promoter.h"

#include <stdint.h>
#include <stdio.h>

#if VDEV_ENABLE_DISPLAY_UI
#include "debugScreen.h"

#define VDEV_UI_BAR_WIDTH 52

static int ui_ready;
static uint64_t last_promotion_frame = UINT64_MAX;

static void set_foreground(uint32_t color)
{
    psvDebugScreenSetFgColor(color);
}

static void begin_frame(void)
{
    psvDebugScreenSetBgColor(UINT32_C(0x101827));
    psvDebugScreenPuts("\e[H\e[2J");
    set_foreground(UINT32_C(0x67E8F9));
    psvDebugScreenPuts("\n    VitaDevDeploy\n");
    set_foreground(UINT32_C(0x94A3B8));
#if VDEV_ENABLE_INSTALL
    psvDebugScreenPuts("    Secure remote installer  |  install enabled\n\n");
#else
    psvDebugScreenPuts("    Secure remote installer  |  verification only\n\n");
#endif
}

static void draw_progress(int percent, int activity_position)
{
    int index;
    int filled;

    const int indeterminate = percent < 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    filled = (percent * VDEV_UI_BAR_WIDTH) / 100;

    set_foreground(UINT32_C(0xCBD5E1));
    psvDebugScreenPuts("    [");
    for (index = 0; index < VDEV_UI_BAR_WIDTH; ++index) {
        if (activity_position >= 0 &&
            index >= activity_position && index < activity_position + 7) {
            set_foreground(UINT32_C(0x67E8F9));
            psvDebugScreenPuts("#");
        } else if (index < filled) {
            set_foreground(UINT32_C(0x22C55E));
            psvDebugScreenPuts("#");
        } else {
            set_foreground(UINT32_C(0x475569));
            psvDebugScreenPuts("-");
        }
    }
    set_foreground(UINT32_C(0xCBD5E1));
    if (indeterminate) {
        psvDebugScreenPuts("]  working\n\n");
    } else {
        psvDebugScreenPrintf("]  %3d%%\n\n", percent);
    }
}

static void draw_footer(void)
{
    set_foreground(UINT32_C(0x64748B));
    psvDebugScreenPuts(
        "\n    Keep Wi-Fi connected. Detailed results are returned to the host.\n"
        "    Do not power off while installation is in progress.\n");
    set_foreground(UINT32_C(0xE2E8F0));
}

int vdev_ui_init(void)
{
    const int result = psvDebugScreenInit();
    if (result < 0) return result;
    ui_ready = 1;
    vdev_ui_status(2, "Starting", "Initializing the deployment agent");
    return 0;
}

void vdev_ui_finish(void)
{
    if (!ui_ready) return;
    psvDebugScreenFinish();
    ui_ready = 0;
}

void vdev_ui_status(int percent, const char *stage, const char *detail)
{
    if (!ui_ready) return;
    begin_frame();
    draw_progress(percent, -1);
    set_foreground(UINT32_C(0xF8FAFC));
    psvDebugScreenPrintf("    %s\n", stage != NULL ? stage : "Working");
    set_foreground(UINT32_C(0x94A3B8));
    psvDebugScreenPrintf("    %s\n", detail != NULL ? detail : "Please wait");
    draw_footer();
}

void vdev_ui_waiting(void)
{
    vdev_ui_status(15, "Waiting for a signed deployment",
                   "Host may upload one authenticated job  |  Circle: close safely");
}

void vdev_ui_promotion_progress(int state, uint64_t elapsed_milliseconds,
                                void *context)
{
    const uint64_t frame = elapsed_milliseconds / UINT64_C(250);
    int position;
    (void)context;
    if (!ui_ready || frame == last_promotion_frame) return;
    last_promotion_frame = frame;
    position = (int)(frame % (VDEV_UI_BAR_WIDTH - 6));

    begin_frame();
    draw_progress(-1, position);
    set_foreground(UINT32_C(0xF8FAFC));
    psvDebugScreenPuts("    Installing package\n");
    set_foreground(UINT32_C(0x94A3B8));
    if (state == VDEV_PROMOTE_PROGRESS_STATE_UNAVAILABLE) {
        psvDebugScreenPrintf(
            "    Waiting for installer status  |  elapsed: %llu s\n",
            (unsigned long long)(elapsed_milliseconds / 1000u));
    } else if (state == VDEV_PROMOTE_PROGRESS_RESULT_UNAVAILABLE) {
        psvDebugScreenPrintf(
            "    Waiting for final install result  |  elapsed: %llu s\n",
            (unsigned long long)(elapsed_milliseconds / 1000u));
    } else {
        psvDebugScreenPrintf(
            "    Vita installer state: %d  |  elapsed: %llu s\n",
            state,
            (unsigned long long)(elapsed_milliseconds / 1000u));
    }
    draw_footer();
}

void vdev_ui_complete(const char *title_id, const char *detail)
{
    if (!ui_ready) return;
    begin_frame();
    draw_progress(100, -1);
    set_foreground(UINT32_C(0x4ADE80));
    psvDebugScreenPuts("    Deployment complete\n");
    set_foreground(UINT32_C(0xE2E8F0));
    psvDebugScreenPrintf("    Title: %s\n", title_id != NULL ? title_id : "unknown");
    set_foreground(UINT32_C(0x94A3B8));
    psvDebugScreenPrintf("    %s\n", detail != NULL ? detail : "Success");
    draw_footer();
}

void vdev_ui_error(const char *stage, int code, const char *detail)
{
    if (!ui_ready) return;
    begin_frame();
    set_foreground(UINT32_C(0xFB7185));
    psvDebugScreenPuts("    Deployment stopped\n\n");
    set_foreground(UINT32_C(0xF8FAFC));
    psvDebugScreenPrintf("    Stage: %s\n", stage != NULL ? stage : "unknown");
    psvDebugScreenPrintf("    Error: %d (0x%08X)\n", code, (unsigned int)code);
    set_foreground(UINT32_C(0xFCA5A5));
    psvDebugScreenPrintf("    %s\n", detail != NULL ? detail : "Unknown failure");
    set_foreground(UINT32_C(0x94A3B8));
    psvDebugScreenPuts(
        "\n    The host result and crash journal contain the same failure.\n"
        "    This screen will remain visible briefly for inspection.\n");
}

#else

/*
 * The production agent is deliberately headless. VitaSDK's debugScreen helper
 * installs a process-owned CDRAM buffer directly with sceDisplaySetFrameBuf.
 * Vita Companion's AppMgr destroy command can terminate the process without
 * running psvDebugScreenFinish(), leaving SceShell to recover an abruptly
 * orphaned display surface. Hardware testing showed that this can wedge the
 * shell. Keep every UI call available as a no-op so validation and installer
 * code do not need display-specific branches.
 */
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
