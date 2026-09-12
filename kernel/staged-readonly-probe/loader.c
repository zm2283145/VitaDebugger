#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "debugScreen.h"
#include "vd_read_ladder_record.h"

#define VD_READ_LADDER_SKPRX_PATH \
    "ux0:app/VDCP00004/module/vd-read-ladder-step.skprx"

static const char* record_path(int slot)
{
    return slot == 0 ? VD_READ_LADDER_RECORD_A_PATH :
                       VD_READ_LADDER_RECORD_B_PATH;
}

static const char* step_name(uint32_t step)
{
    switch(step)
    {
        case VD_READ_STEP_KERNEL_LIFECYCLE: return "kernel lifecycle/no-op";
        case VD_READ_STEP_CPU_ID: return "CPU-ID API";
        case VD_READ_STEP_MIDR: return "MIDR (CP15)";
        case VD_READ_STEP_DIDR: return "DIDR (CP14)";
        case VD_READ_STEP_DSCR: return "DSCR (CP14)";
        case VD_READ_STEP_DBGVCR: return "DBGVCR (CP14)";
        case VD_READ_STEP_BCR0: return "BCR0 (CP14)";
        case VD_READ_STEP_BVR0: return "BVR0 (CP14)";
        case VD_READ_STEP_WCR0: return "WCR0 (CP14)";
        case VD_READ_STEP_WVR0: return "WVR0 (CP14)";
        default: return "invalid";
    }
}

static const char* state_name(uint32_t state)
{
    switch(state)
    {
        case VD_READ_STATE_ATTEMPTED: return "loader armed";
        case VD_READ_STATE_KERNEL_ENTERED: return "kernel entered";
        case VD_READ_STATE_COMPLETE: return "complete";
        default: return "unknown";
    }
}

static const char* result_name(int32_t result)
{
    switch(result)
    {
        case VD_READ_RESULT_OK: return "PASS";
        case VD_READ_RESULT_NOT_RUN: return "not run";
        case VD_READ_RESULT_BAD_RECORD: return "bad record";
        case VD_READ_RESULT_BAD_STEP: return "bad step";
        case VD_READ_RESULT_PREOP_JOURNAL: return "pre-op journal failed";
        case VD_READ_RESULT_POSTOP_JOURNAL: return "post-op journal failed";
        case VD_READ_RESULT_CORE_MIGRATED: return "core changed before guard";
        case VD_READ_RESULT_UNSAFE_CORE: return "system core refused";
        default: return "unknown";
    }
}

static int read_slot(int slot, struct vd_read_ladder_record* record)
{
    SceUID fd = sceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    int result;
    int close_result;

    if(fd < 0)
        return fd;
    result = sceIoRead(fd, record, sizeof(*record));
    close_result = sceIoClose(fd);
    if(result != (int)sizeof(*record))
        return result < 0 ? result : -1;
    if(close_result < 0)
        return close_result;
    return vd_read_ladder_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_read_ladder_record* record, int* slot)
{
    struct vd_read_ladder_record a;
    struct vd_read_ladder_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_b && (!have_a ||
                  vd_read_ladder_revision_newer(b.revision, a.revision)))
    {
        *record = b;
        *slot = 1;
    }
    else
    {
        *record = a;
        *slot = 0;
    }
    return 0;
}

static int write_slot_verified(int slot,
                               struct vd_read_ladder_record* record)
{
    struct vd_read_ladder_record verify;
    SceUID fd;
    int result;
    int close_result;

    record->magic = VD_READ_LADDER_MAGIC;
    record->version = VD_READ_LADDER_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_read_ladder_checksum(record);

    fd = sceIoOpen(record_path(slot),
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if(fd < 0)
        return fd;
    result = sceIoWrite(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
        result = sceIoSyncByFd(fd, 0);
    else if(result >= 0)
        result = -1;
    close_result = sceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result >= 0)
        result = sceIoSync("ux0:", 0);
    if(result < 0)
        return result;
    if(read_slot(slot, &verify) < 0 ||
       verify.revision != record->revision ||
       verify.sequence != record->sequence ||
       verify.step != record->step ||
       verify.state != record->state ||
       verify.checksum != record->checksum)
        return -3;
    return 0;
}

static int write_next(int previous_slot,
                      struct vd_read_ladder_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void print_record(const struct vd_read_ladder_record* record)
{
    psvDebugScreenPrintf("seq=%u rev=%u step=%u %s\n",
                         record->sequence, record->revision,
                         record->step, step_name(record->step));
    psvDebugScreenPrintf("state=%s result=%s (%d) flags=%08X\n",
                         state_name(record->state),
                         result_name(record->result), record->result,
                         record->flags);
    if(record->core_id != UINT32_MAX)
        psvDebugScreenPrintf("expected core=%u\n", record->core_id);
    if((record->flags & VD_READ_FLAG_VALUE_VALID) != 0)
        psvDebugScreenPrintf("value=%08X\n", record->value);
}

static uint32_t next_step_after(const struct vd_read_ladder_record* record,
                                int have_record, int* runnable)
{
    if(!have_record)
    {
        *runnable = 1;
        return VD_READ_STEP_FIRST;
    }
    if(record->state == VD_READ_STATE_COMPLETE &&
       record->result == VD_READ_RESULT_OK &&
       record->step < VD_READ_STEP_LAST)
    {
        *runnable = 1;
        return record->step + 1u;
    }
    *runnable = 0;
    if(record->step >= VD_READ_STEP_FIRST &&
       record->step <= VD_READ_STEP_LAST)
        return record->step;
    return VD_READ_STEP_FIRST;
}

static int is_expected_completion(
    const struct vd_read_ladder_record* record,
    const struct vd_read_ladder_record* attempt)
{
    uint32_t expected_flags = VD_READ_FLAG_VALUE_VALID |
                              VD_READ_FLAG_GENERAL_REGS_ONLY |
                              VD_READ_FLAG_IRQ_GUARDED |
                              VD_READ_FLAG_CORE_MATCH;

    if(attempt->step >= VD_READ_STEP_DIDR)
        expected_flags |= VD_READ_FLAG_CP14;
    return record->revision == attempt->revision + 2u &&
           record->sequence == attempt->sequence &&
           record->step == attempt->step &&
           record->state == VD_READ_STATE_COMPLETE &&
           record->result == VD_READ_RESULT_OK &&
           record->flags == expected_flags &&
           record->core_id <= 2u;
}

static int run_selected_step(uint32_t selected,
                             struct vd_read_ladder_record* latest,
                             int* latest_slot, int have_latest)
{
    struct vd_read_ladder_record attempt = {0};
    uint32_t next_revision = 1;
    uint32_t next_sequence = 1;
    SceUID module;
    tai_module_args_t arguments;
    int start_result = -1;
    int start_call;
    int lifecycle_ok = 0;
    int result;

    if(have_latest)
    {
        /* Reserve two further revisions for kernel-entered and completion. */
        if(latest->revision > UINT32_MAX - 3u ||
           latest->sequence == UINT32_MAX)
            return -10;
        next_revision = latest->revision + 1u;
        next_sequence = latest->sequence + 1u;
    }

    attempt.revision = next_revision;
    attempt.sequence = next_sequence;
    attempt.step = selected;
    attempt.state = VD_READ_STATE_ATTEMPTED;
    attempt.result = VD_READ_RESULT_NOT_RUN;
    attempt.flags = VD_READ_FLAG_GENERAL_REGS_ONLY;
    attempt.core_id = UINT32_MAX;
    if(selected >= VD_READ_STEP_DIDR)
        attempt.flags |= VD_READ_FLAG_CP14;

    result = write_next(have_latest ? *latest_slot : -1,
                        &attempt, latest_slot);
    if(result < 0)
    {
        psvDebugScreenPrintf("Journal failed: %08X\n", (uint32_t)result);
        psvDebugScreenPrintf("Kernel module was NOT loaded.\n");
        return result;
    }
    *latest = attempt;
    psvDebugScreenPrintf("Armed durably; loading one-shot module...\n");

    module = taiLoadKernelModule(VD_READ_LADDER_SKPRX_PATH, 0, NULL);
    psvDebugScreenPrintf("load=%08X\n", (uint32_t)module);
    if(module < 0)
        return module;

    arguments.size = sizeof(arguments);
    arguments.pid = KERNEL_PID;
    arguments.args = 0;
    arguments.argp = NULL;
    arguments.flags = 0;
    start_call = taiStartKernelModuleForUser(module, &arguments, NULL,
                                              &start_result);
    psvDebugScreenPrintf("start=%08X result=%08X\n",
                         (uint32_t)start_call, (uint32_t)start_result);
    if(start_call < 0)
    {
        const int unload_result = taiUnloadKernelModule(module, 0, NULL);
        psvDebugScreenPrintf("start failed; unload=%08X\n",
                             (uint32_t)unload_result);
        if(unload_result < 0)
            psvDebugScreenPrintf("Residency uncertain: reboot before retry.\n");
    }
    else if(start_result != SCE_KERNEL_START_NO_RESIDENT)
    {
        int stop_result = -1;
        const int cleanup_result = taiStopUnloadKernelModuleForUser(
            module, &arguments, NULL, &stop_result);
        psvDebugScreenPrintf("unexpected residency; cleanup=%08X/%08X\n",
                             (uint32_t)cleanup_result,
                             (uint32_t)stop_result);
        if(cleanup_result < 0)
            psvDebugScreenPrintf("Reboot before another probe.\n");
    }
    else
    {
        lifecycle_ok = 1;
    }

    if(read_latest(latest, latest_slot) < 0)
    {
        psvDebugScreenPrintf("No valid post-run journal.\n");
        return -11;
    }
    print_record(latest);
    if(!lifecycle_ok)
    {
        psvDebugScreenPrintf("Lifecycle invariant failed; ladder locked.\n");
        return -12;
    }
    if(!is_expected_completion(latest, &attempt))
    {
        psvDebugScreenPrintf("Completion does not match this attempt.\n");
        return -13;
    }
    return 0;
}

int main(void)
{
    SceCtrlData previous = {0};
    SceCtrlData pad = {0};
    struct vd_read_ladder_record latest;
    int latest_slot = -1;
    int have_latest;
    int runnable;
    uint32_t selected;

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlPeekBufferPositive(0, &previous, 1);
    sceIoMkdir("ux0:data/VitaDebugger", 0777);
    have_latest = read_latest(&latest, &latest_slot) == 0;
    selected = next_step_after(&latest, have_latest, &runnable);

    psvDebugScreenPrintf("VitaDebugger staged read-only ladder\n\n");
    psvDebugScreenPrintf("No CP14 writes. No hooks. Never resident.\n");
    psvDebugScreenPrintf("Each accepted X edge runs exactly one rung.\n");
    psvDebugScreenPrintf("Rungs are sequential and cannot be skipped.\n\n");
    if(have_latest)
    {
        psvDebugScreenPrintf("Newest durable record:\n");
        print_record(&latest);
        if(latest.state != VD_READ_STATE_COMPLETE)
        {
            psvDebugScreenPrintf("INCOMPLETE: the shown rung may have faulted.\n");
            psvDebugScreenPrintf("The ladder is locked; inspect its journal.\n");
        }
    }
    else
    {
        psvDebugScreenPrintf("No prior read-ladder journal.\n");
    }
    psvDebugScreenPrintf("\nX runs the next rung; O exits.\n");
    psvDebugScreenPrintf("Current %u: %s [%s]\n", selected,
                         step_name(selected),
                         runnable ? "READY" : "LOCKED");

    for(;;)
    {
        uint32_t pressed;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~previous.buttons;
        previous = pad;

        if((pressed & SCE_CTRL_CIRCLE) != 0)
            return 0;
        if((pressed & SCE_CTRL_CROSS) != 0)
        {
            if(!runnable)
            {
                psvDebugScreenPrintf("Locked: inspect/clear both journals first.\n");
                sceKernelDelayThread(16000);
                continue;
            }
            psvDebugScreenPrintf("\nRunning %u: %s\n", selected,
                                 step_name(selected));
            if(run_selected_step(selected, &latest, &latest_slot,
                                 have_latest) >= 0)
            {
                have_latest = 1;
                if(selected < VD_READ_STEP_LAST)
                {
                    selected++;
                    runnable = 1;
                    psvDebugScreenPrintf("Next %u: %s\n", selected,
                                         step_name(selected));
                }
                else
                {
                    runnable = 0;
                    psvDebugScreenPrintf("All read-only rungs complete.\n");
                }
            }
            else
            {
                have_latest = read_latest(&latest, &latest_slot) == 0;
                runnable = 0;
                psvDebugScreenPrintf("Stopped on this rung; inspect journal.\n");
            }
        }
        sceKernelDelayThread(16000);
    }
}
