#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "debugScreen.h"
#include "vd_dipsw_probe_record.h"

#define VD_DIPSW_PROBE_SKPRX_PATH \
    "ux0:app/VDCP00005/module/vd-dipsw-read-probe.skprx"

static const char* record_path(int slot)
{
    return slot == 0 ? VD_DIPSW_PROBE_RECORD_A_PATH :
                       VD_DIPSW_PROBE_RECORD_B_PATH;
}

static const char* state_name(uint32_t state)
{
    switch(state)
    {
        case VD_DIPSW_PROBE_STATE_ATTEMPTED: return "loader armed";
        case VD_DIPSW_PROBE_STATE_KERNEL_ENTERED: return "kernel entered";
        case VD_DIPSW_PROBE_STATE_COMPLETE: return "complete";
        default: return "unknown";
    }
}

static const char* result_name(int32_t result)
{
    switch(result)
    {
        case VD_DIPSW_PROBE_OK: return "PASS";
        case VD_DIPSW_PROBE_NOT_RUN: return "not run";
        case VD_DIPSW_PROBE_ERROR_CHECK_203:
            return "bit 203 check returned non-boolean";
        case VD_DIPSW_PROBE_ERROR_CHECK_228:
            return "bit 228 check returned non-boolean";
        case VD_DIPSW_PROBE_ERROR_MISMATCH_203:
            return "bit 203 differs from raw word";
        case VD_DIPSW_PROBE_ERROR_MISMATCH_228:
            return "bit 228 differs from raw word";
        default: return "unknown";
    }
}

static int read_slot(int slot, struct vd_dipsw_probe_record* record)
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
    return vd_dipsw_probe_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_dipsw_probe_record* record, int* slot)
{
    struct vd_dipsw_probe_record a;
    struct vd_dipsw_probe_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_a && have_b && a.revision == b.revision)
        return -3;
    if(have_b && (!have_a ||
                  vd_dipsw_probe_revision_newer(b.revision, a.revision)))
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

static int journal_file_present(void)
{
    SceIoStat stat;
    return sceIoGetstat(VD_DIPSW_PROBE_RECORD_A_PATH, &stat) >= 0 ||
           sceIoGetstat(VD_DIPSW_PROBE_RECORD_B_PATH, &stat) >= 0;
}

static int write_slot_verified(int slot,
                               struct vd_dipsw_probe_record* record)
{
    struct vd_dipsw_probe_record verify;
    SceUID fd;
    int result;
    int close_result;

    record->magic = VD_DIPSW_PROBE_MAGIC;
    record->version = VD_DIPSW_PROBE_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_dipsw_probe_checksum(record);

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
       verify.state != record->state ||
       verify.checksum != record->checksum)
        return -3;
    return 0;
}

static int write_next(int previous_slot,
                      struct vd_dipsw_probe_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void print_record(const struct vd_dipsw_probe_record* record)
{
    const uint32_t cp_version = record->cp_build_version & UINT32_C(0xffff);
    const uint32_t cp_build = record->cp_build_version >> 16;
    const uint32_t derived_203 =
        (record->debug_control & VD_DIPSW_RECONFIG_MASK) != 0;
    const uint32_t derived_228 =
        (record->system_control & VD_DIPSW_HW_DEBUG_MASK) != 0;

    psvDebugScreenPrintf("seq=%u rev=%u state=%s\n",
                         record->sequence, record->revision,
                         state_name(record->state));
    psvDebugScreenPrintf("result=%s (%d) flags=%08X core=%u\n",
                         result_name(record->result), record->result,
                         record->flags, record->core_id);
    if(record->state != VD_DIPSW_PROBE_STATE_COMPLETE)
        return;

    psvDebugScreenPrintf("CP version=%04X build ID=%04X raw=%08X\n",
                         cp_version, cp_build, record->cp_build_version);
    psvDebugScreenPrintf("DEBUG=%08X SYSTEM=%08X\n",
                         record->debug_control, record->system_control);
    psvDebugScreenPrintf("bit203 direct/word=%d/%u %s\n",
                         record->check_203, derived_203,
                         record->check_203 == (int32_t)derived_203 ?
                             "MATCH" : "MISMATCH");
    psvDebugScreenPrintf("bit228 direct/word=%d/%u %s\n",
                         record->check_228, derived_228,
                         record->check_228 == (int32_t)derived_228 ?
                             "MATCH" : "MISMATCH");
}

static int expected_completion(const struct vd_dipsw_probe_record* record,
                               const struct vd_dipsw_probe_record* attempt)
{
    return record->revision == attempt->revision + 2u &&
           record->sequence == attempt->sequence &&
           record->state == VD_DIPSW_PROBE_STATE_COMPLETE &&
           record->result == VD_DIPSW_PROBE_OK &&
           record->flags == VD_DIPSW_PROBE_FLAGS_COMPLETE;
}

static int run_probe(struct vd_dipsw_probe_record* latest,
                     int* latest_slot, int have_latest)
{
    struct vd_dipsw_probe_record attempt = {0};
    tai_module_args_t arguments;
    SceUID module;
    int start_result = -1;
    int start_call;
    int lifecycle_ok = 0;
    int result;

    attempt.revision = 1;
    attempt.sequence = 1;
    if(have_latest)
    {
        if(latest->revision > UINT32_MAX - 3u ||
           latest->sequence == UINT32_MAX)
            return -10;
        attempt.revision = latest->revision + 1u;
        attempt.sequence = latest->sequence + 1u;
    }
    attempt.state = VD_DIPSW_PROBE_STATE_ATTEMPTED;
    attempt.result = VD_DIPSW_PROBE_NOT_RUN;
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

    module = taiLoadKernelModule(VD_DIPSW_PROBE_SKPRX_PATH, 0, NULL);
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
        return -12;
    if(!expected_completion(latest, &attempt))
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
    struct vd_dipsw_probe_record latest;
    int latest_slot = -1;
    int have_latest;
    int journal_error;
    int runnable;
    int ran = 0;

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlPeekBufferPositive(0, &previous, 1);
    (void)sceIoMkdir("ux0:data/VitaDebugger", 0777);
    const int latest_result = read_latest(&latest, &latest_slot);
    have_latest = latest_result == 0;
    journal_error = !have_latest &&
        (latest_result == -3 || journal_file_present());
    runnable = !journal_error && (!have_latest ||
        (latest.state == VD_DIPSW_PROBE_STATE_COMPLETE &&
         latest.result == VD_DIPSW_PROBE_OK &&
         latest.flags == VD_DIPSW_PROBE_FLAGS_COMPLETE));

    psvDebugScreenPrintf("VitaDebugger DIP-switch read probe\n\n");
    psvDebugScreenPrintf("READ ONLY: documented DIP getter APIs.\n");
    psvDebugScreenPrintf("No set/clear, CP14, hooks, or residency.\n\n");
    if(have_latest)
    {
        psvDebugScreenPrintf("Newest durable record:\n");
        print_record(&latest);
        if(!runnable)
        {
            psvDebugScreenPrintf("Incomplete/failed record; probe locked.\n");
            psvDebugScreenPrintf("Inspect both journal slots before clearing.\n");
        }
    }
    else if(journal_error)
    {
        psvDebugScreenPrintf("Journal conflict/corruption; probe locked.\n");
        psvDebugScreenPrintf("Pull both slots before clearing them.\n");
    }
    else
    {
        psvDebugScreenPrintf("No prior DIP-switch journal.\n");
    }
    psvDebugScreenPrintf("\nX records one fresh sample; O exits.\n");

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
            if(ran)
                psvDebugScreenPrintf("Already ran; relaunch for a fresh read.\n");
            else if(!runnable)
                psvDebugScreenPrintf("Locked: inspect the journal first.\n");
            else
            {
                ran = 1;
                psvDebugScreenPrintf("\nCollecting read-only state...\n");
                if(run_probe(&latest, &latest_slot, have_latest) >= 0)
                    psvDebugScreenPrintf("Read-only inventory complete.\n");
                else
                    psvDebugScreenPrintf("Probe failed and is now locked.\n");
            }
        }
        sceKernelDelayThread(16000);
    }
}
