#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include "debugScreen.h"
#include "vitadebug_pmu_probe.h"

static const char* stage_name(uint32_t stage)
{
    switch(stage)
    {
        case VD_PMU_BACKEND_TEST_NOT_RUN: return "not run";
        case VD_PMU_BACKEND_TEST_SNAPSHOT: return "snapshot";
        case VD_PMU_BACKEND_TEST_CONFIGURE: return "configure";
        case VD_PMU_BACKEND_TEST_INCREMENT: return "increment";
        case VD_PMU_BACKEND_TEST_READ: return "read";
        case VD_PMU_BACKEND_TEST_RESTORE: return "restore";
        case VD_PMU_BACKEND_TEST_VERIFY: return "verify";
        case VD_PMU_BACKEND_TEST_COMPLETE: return "complete";
        default: return "unknown";
    }
}

static const char* record_state_name(uint32_t state)
{
    switch(state)
    {
        case VD_PMU_PROBE_STATE_ATTEMPTED: return "armed";
        case VD_PMU_PROBE_STATE_KERNEL_ENTERED: return "kernel entered";
        case VD_PMU_PROBE_STATE_RECOVERY_STARTED: return "recovering";
        case VD_PMU_PROBE_STATE_COMPLETE: return "complete";
        case VD_PMU_PROBE_STATE_RESTORE_REQUIRED:
            return "RESTORE REQUIRED";
        default: return "unknown";
    }
}

static const char* probe_result_name(int result)
{
    switch(result)
    {
        case 0: return "PASS";
        case VD_PMU_PROBE_ERROR_INVALID: return "invalid request";
        case VD_PMU_PROBE_ERROR_DISABLED: return "backend disabled";
        case VD_PMU_PROBE_ERROR_BUSY: return "busy";
        case VD_PMU_PROBE_ERROR_JOURNAL: return "journal failure";
        case VD_PMU_PROBE_ERROR_LOCKED: return "journal safety lock";
        case VD_PMU_PROBE_ERROR_CORE_ORDER:
            return "core 0 must pass first";
        case VD_PMU_PROBE_ERROR_RESTORE_REQUIRED:
            return "restore required";
        case VD_PMU_PROBE_ERROR_NO_RUNTIME_RESTORE:
            return "no in-memory restore record";
        case VD_PMU_PROBE_ERROR_REVISION: return "journal exhausted";
        case VD_PMU_BACKEND_ERROR_INVALID: return "backend invalid";
        case VD_PMU_BACKEND_ERROR_DISABLED: return "backend not ready";
        case VD_PMU_BACKEND_ERROR_BUSY: return "backend busy";
        case VD_PMU_BACKEND_ERROR_CORE: return "core/CPU mismatch";
        case VD_PMU_BACKEND_ERROR_TIMEOUT: return "worker timeout";
        case VD_PMU_BACKEND_ERROR_NOT_IDLE: return "PMU not idle";
        case VD_PMU_BACKEND_ERROR_CONFLICT: return "external PMU conflict";
        case VD_PMU_BACKEND_ERROR_VERIFY: return "read-back mismatch";
        case VD_PMU_BACKEND_ERROR_RESTORE: return "restore failed";
        case VD_PMU_BACKEND_ERROR_CLEANUP: return "worker cleanup failed";
        default: return "error";
    }
}

static void clear_screen(void)
{
    psvDebugScreenPrintf("\x1b[2J\x1b[H");
}

static void print_snapshot_pair(
    const struct vd_pmu_backend_test_result* test)
{
    const struct vd_pmu_snapshot* before = &test->before;
    const struct vd_pmu_snapshot* after = &test->after;
    const uint32_t lane = VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS - 1u;

    psvDebugScreenPrintf("raw register                 before     after\n");
    psvDebugScreenPrintf("PMCR                       %08X  %08X\n",
                         before->raw_pmcr, after->raw_pmcr);
    psvDebugScreenPrintf("PMCNTEN                     %08X  %08X\n",
                         before->raw_pmcntenset,
                         after->raw_pmcntenset);
    psvDebugScreenPrintf("PMINTEN                     %08X  %08X\n",
                         before->raw_pmintenset,
                         after->raw_pmintenset);
    psvDebugScreenPrintf("PMOVSR                      %08X  %08X\n",
                         before->raw_pmovsr, after->raw_pmovsr);
    psvDebugScreenPrintf("PMSELR                      %08X  %08X\n",
                         before->raw_pmselr, after->raw_pmselr);
    psvDebugScreenPrintf("PMUSERENR                   %08X  %08X\n",
                         before->raw_pmuserenr,
                         after->raw_pmuserenr);
    psvDebugScreenPrintf("PMCCNTR                     %08X  %08X\n",
                         before->raw_pmccntr, after->raw_pmccntr);
    psvDebugScreenPrintf("lane5 type/count            %08X/%08X  %08X/%08X\n",
                         before->raw_pmxevtyper[lane],
                         before->raw_pmxevcntr[lane],
                         after->raw_pmxevtyper[lane],
                         after->raw_pmxevcntr[lane]);
}

static void print_test_result(
    int syscall_result, const struct vd_pmu_backend_test_result* test)
{
    psvDebugScreenPrintf("\nExpected stages:\n");
    psvDebugScreenPrintf(
        "snapshot > configure > +17 > read > restore > verify\n");
    psvDebugScreenPrintf("Reached: %s (%u)\n",
                         stage_name(test->stage), test->stage);
    psvDebugScreenPrintf("call=%08X %s  operation=%08X  restore=%08X\n",
                         (uint32_t)syscall_result,
                         probe_result_name(syscall_result),
                         (uint32_t)test->operation_result,
                         (uint32_t)test->restore_result);
    psvDebugScreenPrintf("MIDR=%08X MPIDR=%08X core=%u\n",
                         test->raw_midr, test->raw_mpidr, test->core_id);
    psvDebugScreenPrintf("software increments observed/expected=%u/%u\n",
                         test->observed_count, test->increment_count);
    print_snapshot_pair(test);
    psvDebugScreenPrintf("Exact restoration: %s\n",
                         vdPmuProbeRestorationProven(test) ?
                             "PROVEN" : "NOT PROVEN");
    psvDebugScreenPrintf("Gate result: %s\n",
                         syscall_result == 0 &&
                         vdPmuProbeTestPassed(test) ? "PASS" : "FAIL");
}

static void print_record(const struct vd_pmu_probe_record* record)
{
    psvDebugScreenPrintf("Journal seq=%u rev=%u state=%s\n",
                         record->sequence, record->revision,
                         record_state_name(record->state));
    psvDebugScreenPrintf("call=%08X restore=%08X flags=%08X cores=%X\n",
                         (uint32_t)record->syscall_result,
                         (uint32_t)record->test.restore_result,
                         record->flags, record->passed_core_mask);
    if((record->flags & VD_PMU_PROBE_FLAG_HAS_RESULT) != 0)
        psvDebugScreenPrintf("last core=%u stage=%s count=%u/%u\n",
                             record->requested_core,
                             stage_name(record->test.stage),
                             record->test.observed_count,
                             record->test.increment_count);
}

static int get_status(struct vd_pmu_probe_status* status)
{
    const int result = vdPmuProbeGetStatus(status);
    if(result < 0)
        psvDebugScreenPrintf("status syscall=%08X\n", (uint32_t)result);
    return result;
}

static void render_status(uint32_t selected_core,
                          struct vd_pmu_probe_status* status)
{
    clear_screen();
    psvDebugScreenPrintf("VitaDebugger isolated PMU session gate\n");
    psvDebugScreenPrintf("Cortex-A9 event 0x00 / lane 5 / exactly 17 writes\n");
    psvDebugScreenPrintf("No PMU register is touched merely by opening this UI.\n\n");

    if(get_status(status) < 0)
    {
        psvDebugScreenPrintf("Kernel probe unavailable. Check *KERNEL config.\n");
        psvDebugScreenPrintf("O exits.\n");
        return;
    }
    psvDebugScreenPrintf(
        "backend start=%08X ready=%u obligation=%u journal=%d lock=%u\n",
        (uint32_t)status->module_start_result, status->backend_ready,
        status->restore_obligation, status->journal_result,
        status->locked);
    if(status->journal_result == VD_PMU_PROBE_JOURNAL_OK)
        print_record(&status->latest);
    else if(status->journal_result == VD_PMU_PROBE_JOURNAL_EMPTY)
        psvDebugScreenPrintf("Journal empty: core 0 is the only first run.\n");
    else
        psvDebugScreenPrintf("Journal corrupt/conflicted: testing is locked.\n");

    psvDebugScreenPrintf("\nSelected core: %u (core 0 must pass first)\n",
                         selected_core);
    psvDebugScreenPrintf("X run once   Left/Right select   Triangle recover\n");
    psvDebugScreenPrintf("O exit (the kernel module remains resident)\n");
    if(status->locked)
        psvDebugScreenPrintf(
            "LOCKED: retain/pull both journal files before clearing.\n");
}

int main(void)
{
    SceCtrlData previous = {0};
    SceCtrlData pad = {0};
    struct vd_pmu_probe_status status;
    uint32_t selected_core = 0;
    int ran = 0;

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlPeekBufferPositive(0, &previous, 1);
    render_status(selected_core, &status);

    for(;;)
    {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const uint32_t pressed = pad.buttons & ~previous.buttons;
        previous = pad;

        if((pressed & SCE_CTRL_CIRCLE) != 0)
            return 0;
        if((pressed & SCE_CTRL_LEFT) != 0 && selected_core > 0)
        {
            selected_core--;
            render_status(selected_core, &status);
        }
        if((pressed & SCE_CTRL_RIGHT) != 0 &&
           selected_core + 1u < VD_PMU_BACKEND_APP_CORE_COUNT)
        {
            selected_core++;
            render_status(selected_core, &status);
        }
        if((pressed & SCE_CTRL_TRIANGLE) != 0)
        {
            clear_screen();
            psvDebugScreenPrintf("Requesting exact backend recovery...\n");
            const int result = vdPmuProbeRecover();
            psvDebugScreenPrintf("recovery=%08X %s\n",
                                 (uint32_t)result,
                                 probe_result_name(result));
            sceKernelDelayThread(750000);
            render_status(selected_core, &status);
        }
        if((pressed & SCE_CTRL_CROSS) != 0)
        {
            if(ran)
            {
                psvDebugScreenPrintf(
                    "\nAlready ran this launch; relaunch before another gate.\n");
            }
            else
            {
                struct vd_pmu_backend_test_result test;
                ran = 1;
                clear_screen();
                psvDebugScreenPrintf("Running bounded PMU gate on core %u...\n",
                                     selected_core);
                psvDebugScreenPrintf(
                    "Do not close the app until the result appears.\n");
                const int result =
                    vdPmuProbeRunSelfTest(selected_core, &test);
                print_test_result(result, &test);
                if(get_status(&status) >= 0)
                    psvDebugScreenPrintf(
                        "backend ready=%u obligation=%u journal=%d lock=%u\n",
                        status.backend_ready, status.restore_obligation,
                        status.journal_result, status.locked);
                psvDebugScreenPrintf(
                    "\nBoth journal slots remain under ux0:data/VitaDebugger.\n");
                psvDebugScreenPrintf(
                    "O exits; the kernel probe is never hot-unloaded here.\n");
            }
        }
        sceKernelDelayThread(16000);
    }
}
