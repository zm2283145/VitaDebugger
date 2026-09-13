#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "debugScreen.h"
#include "vd_dipsw_set_probe_record.h"

#define VD_DIPSW_SET_PROBE_SKPRX_PATH \
    "ux0:app/VDCP00006/module/vd-dipsw-set-restore-probe.skprx"
#define VD_SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)

static const char* record_path(int slot)
{
    return slot == 0 ? VD_DIPSW_SET_PROBE_RECORD_A_PATH :
                       VD_DIPSW_SET_PROBE_RECORD_B_PATH;
}

static const char* state_name(uint32_t state)
{
    switch(state)
    {
        case VD_DIPSW_SET_PROBE_STATE_ATTEMPTED: return "loader armed";
        case VD_DIPSW_SET_PROBE_STATE_KERNEL_ENTERED: return "kernel entered";
        case VD_DIPSW_SET_PROBE_STATE_ORIGINAL_CAPTURED:
            return "original captured";
        case VD_DIPSW_SET_PROBE_STATE_SET_PENDING: return "set pending";
        case VD_DIPSW_SET_PROBE_STATE_COMPLETE: return "complete";
        default: return "unknown";
    }
}

static const char* result_name(int32_t result)
{
    switch(result)
    {
        case VD_DIPSW_SET_PROBE_OK: return "PASS";
        case VD_DIPSW_SET_PROBE_NOT_RUN: return "not run";
        case VD_DIPSW_SET_PROBE_ERROR_BEFORE_CHECK_203:
            return "initial bit 203 was not boolean";
        case VD_DIPSW_SET_PROBE_ERROR_BEFORE_CHECK_228:
            return "initial bit 228 was not boolean";
        case VD_DIPSW_SET_PROBE_ERROR_BEFORE_MISMATCH_203:
            return "initial bit 203 disagreed with word";
        case VD_DIPSW_SET_PROBE_ERROR_BEFORE_MISMATCH_228:
            return "initial bit 228 disagreed with word";
        case VD_DIPSW_SET_PROBE_ERROR_RECONFIG_DISABLED:
            return "bit 203 blocked mutation";
        case VD_DIPSW_SET_PROBE_ERROR_228_ALREADY_SET:
            return "bit 228 was already set";
        case VD_DIPSW_SET_PROBE_ERROR_BASELINE_UNSTABLE:
            return "two initial reads differed";
        case VD_DIPSW_SET_PROBE_ERROR_SET_CHECK_228:
            return "bit 228 did not read as one";
        case VD_DIPSW_SET_PROBE_ERROR_SET_SYSTEM_DELTA:
            return "set changed an unexpected system bit";
        case VD_DIPSW_SET_PROBE_ERROR_RESTORE_CP_CHANGED:
            return "CP word did not restore exactly";
        case VD_DIPSW_SET_PROBE_ERROR_RESTORE_CHECK_203:
            return "bit 203 did not restore exactly";
        case VD_DIPSW_SET_PROBE_ERROR_RESTORE_CHECK_228:
            return "bit 228 did not restore exactly";
        case VD_DIPSW_SET_PROBE_ERROR_RESTORE_DEBUG_CHANGED:
            return "debug word did not restore exactly";
        case VD_DIPSW_SET_PROBE_ERROR_RESTORE_SYSTEM_CHANGED:
            return "system word did not restore exactly";
        case VD_DIPSW_SET_PROBE_ERROR_JOURNAL_ORIGINAL:
            return "could not journal original state";
        case VD_DIPSW_SET_PROBE_ERROR_JOURNAL_SET_PENDING:
            return "could not journal before setter";
        default: return "unknown";
    }
}

static int read_slot(int slot, struct vd_dipsw_set_probe_record* record)
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
    return vd_dipsw_set_probe_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_dipsw_set_probe_record* record, int* slot)
{
    struct vd_dipsw_set_probe_record a;
    struct vd_dipsw_set_probe_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_a && have_b && a.revision == b.revision)
        return -3;
    if(have_b && (!have_a ||
                  vd_dipsw_set_probe_revision_newer(b.revision, a.revision)))
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

static int journal_slot_presence(const char* path, int* present)
{
    SceIoStat stat;
    const int result = sceIoGetstat(path, &stat);

    if(result >= 0)
    {
        *present = 1;
        return 0;
    }
    if((uint32_t)result == VD_SCE_ERROR_ERRNO_ENOENT)
    {
        *present = 0;
        return 0;
    }

    /* An indeterminate filesystem result must never authorize mutation. */
    *present = 1;
    return result;
}

static int journal_files_present(int* present)
{
    int present_a = 1;
    int present_b = 1;
    const int result_a = journal_slot_presence(
        VD_DIPSW_SET_PROBE_RECORD_A_PATH, &present_a);
    const int result_b = journal_slot_presence(
        VD_DIPSW_SET_PROBE_RECORD_B_PATH, &present_b);

    *present = present_a || present_b;
    if(result_a < 0)
        return result_a;
    if(result_b < 0)
        return result_b;
    return 0;
}

static int write_slot_verified(int slot,
                               struct vd_dipsw_set_probe_record* record)
{
    struct vd_dipsw_set_probe_record verify;
    SceUID fd;
    int result;
    int close_result;

    record->magic = VD_DIPSW_SET_PROBE_MAGIC;
    record->version = VD_DIPSW_SET_PROBE_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_dipsw_set_probe_checksum(record);

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
                      struct vd_dipsw_set_probe_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void print_record(const struct vd_dipsw_set_probe_record* record)
{
    psvDebugScreenPrintf("seq=%u rev=%u state=%s\n",
                         record->sequence, record->revision,
                         state_name(record->state));
    psvDebugScreenPrintf("primary=%s (%d) restore=%s (%d)\n",
                         result_name(record->result), record->result,
                         result_name(record->restore_result),
                         record->restore_result);
    psvDebugScreenPrintf("flags=%08X core=%u\n",
                         record->flags, record->core_id);
    psvDebugScreenPrintf("hazard=%u calls=%u/%u journal=%08X\n",
                         record->hazard_armed,
                         record->set_call_count,
                         record->clear_call_count,
                         (uint32_t)record->journal_error);
    if(record->state >= VD_DIPSW_SET_PROBE_STATE_ORIGINAL_CAPTURED)
    {
        psvDebugScreenPrintf("before:  CP=%08X D=%08X S=%08X\n",
                             record->before_cp_build_version,
                             record->before_debug, record->before_system);
        psvDebugScreenPrintf("         b203=%d b228=%d\n",
                             record->before_check_203,
                             record->before_check_228);
        psvDebugScreenPrintf("confirm: CP=%08X D=%08X S=%08X\n",
                             record->confirm_cp_build_version,
                             record->confirm_debug, record->confirm_system);
        psvDebugScreenPrintf("         b203=%d b228=%d\n",
                             record->confirm_check_203,
                             record->confirm_check_228);
    }
    if(record->state == VD_DIPSW_SET_PROBE_STATE_COMPLETE &&
       record->set_call_count != 0)
        psvDebugScreenPrintf("set:     S=%08X b228=%d\n",
                             record->after_set_system,
                             record->after_set_check_228);
    if(record->state == VD_DIPSW_SET_PROBE_STATE_COMPLETE &&
       record->clear_call_count != 0)
    {
        psvDebugScreenPrintf("restore: CP=%08X D=%08X S=%08X\n",
                             record->after_restore_cp_build_version,
                             record->after_restore_debug,
                             record->after_restore_system);
        psvDebugScreenPrintf("         b203=%d b228=%d\n",
                             record->after_restore_check_203,
                             record->after_restore_check_228);
    }
}

static int completed_exactly(const struct vd_dipsw_set_probe_record* record,
                             const struct vd_dipsw_set_probe_record* attempt)
{
    return record->revision == attempt->revision + 4u &&
           record->sequence == attempt->sequence &&
           record->state == VD_DIPSW_SET_PROBE_STATE_COMPLETE &&
           record->result == VD_DIPSW_SET_PROBE_OK &&
           record->restore_result == VD_DIPSW_SET_PROBE_OK &&
           record->flags == VD_DIPSW_SET_FLAGS_COMPLETE &&
           record->hazard_armed == 1 &&
           record->set_call_count == 1 &&
           record->clear_call_count == 1 &&
           record->journal_error == 0;
}

static int run_probe(struct vd_dipsw_set_probe_record* latest,
                     int* latest_slot, int have_latest)
{
    struct vd_dipsw_set_probe_record attempt = {0};
    tai_module_args_t arguments;
    SceUID module;
    int start_result = -1;
    int start_call;
    int lifecycle_ok = 0;
    int journal_present = 1;
    int result;

    result = journal_files_present(&journal_present);
    if(have_latest || result < 0 || journal_present)
    {
        psvDebugScreenPrintf("Journal gate changed; kernel NOT loaded.\n");
        return -25;
    }

    attempt.revision = 1;
    attempt.sequence = 1;
    if(have_latest)
    {
        if(latest->revision > UINT32_MAX - 5u ||
           latest->sequence == UINT32_MAX)
            return -20;
        attempt.revision = latest->revision + 1u;
        attempt.sequence = latest->sequence + 1u;
    }
    attempt.state = VD_DIPSW_SET_PROBE_STATE_ATTEMPTED;
    attempt.result = VD_DIPSW_SET_PROBE_NOT_RUN;
    attempt.restore_result = VD_DIPSW_SET_PROBE_NOT_RUN;
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

    module = taiLoadKernelModule(VD_DIPSW_SET_PROBE_SKPRX_PATH, 0, NULL);
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
    }
    else if(start_result != SCE_KERNEL_START_NO_RESIDENT)
    {
        int stop_result = -1;
        const int cleanup_result = taiStopUnloadKernelModuleForUser(
            module, &arguments, NULL, &stop_result);
        psvDebugScreenPrintf("unexpected residency; cleanup=%08X/%08X\n",
                             (uint32_t)cleanup_result,
                             (uint32_t)stop_result);
    }
    else
    {
        lifecycle_ok = 1;
    }

    /*
     * Do not accept a COMPLETE record until a user-side volume sync also
     * succeeds. This is deliberately separate from the kernel writer's fd
     * and volume syncs: a cached read alone is not durable proof.
     */
    result = sceIoSync("ux0:", 0);
    if(result < 0)
    {
        psvDebugScreenPrintf("Post-run volume sync failed: %08X\n",
                             (uint32_t)result);
        psvDebugScreenPrintf("Do not rerun; REBOOT and pull both slots.\n");
        return -24;
    }

    if(read_latest(latest, latest_slot) < 0)
    {
        psvDebugScreenPrintf("No valid post-run journal. REBOOT NOW.\n");
        return -21;
    }
    print_record(latest);
    if(!lifecycle_ok)
    {
        psvDebugScreenPrintf("Lifecycle uncertain. REBOOT NOW.\n");
        return -22;
    }
    if(!completed_exactly(latest, &attempt))
    {
        psvDebugScreenPrintf("Exact set/restore proof FAILED.\n");
        psvDebugScreenPrintf("Do not rerun; REBOOT and pull both slots.\n");
        return -23;
    }
    return 0;
}

int main(void)
{
    SceCtrlData previous = {0};
    SceCtrlData pad = {0};
    struct vd_dipsw_set_probe_record latest;
    int latest_slot = -1;
    int have_latest;
    int journal_error;
    int journal_present;
    int journal_status;
    int runnable;
    int ran = 0;

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlPeekBufferPositive(0, &previous, 1);
    (void)sceIoMkdir("ux0:data/VitaDebugger", 0777);
    const int latest_result = read_latest(&latest, &latest_slot);
    have_latest = latest_result == 0;
    journal_present = 1;
    journal_status = journal_files_present(&journal_present);
    journal_error = journal_status < 0 ||
        (journal_present && !have_latest);

    /*
     * This is a deliberately one-shot experiment. Any existing slot,
     * including a valid PASS beside a torn peer, locks future mutation until
     * both files have been pulled and explicitly removed by the host.
     */
    runnable = journal_status == 0 && latest_result == -1 &&
        !have_latest && !journal_present;

    psvDebugScreenPrintf("VitaDebugger DIP 228 set/restore probe\n\n");
    psvDebugScreenPrintf("MUTATES cached bit 228 once, then clears it.\n");
    psvDebugScreenPrintf("No bit 203 write, CP14, hook, or residency.\n\n");
    if(have_latest)
    {
        psvDebugScreenPrintf("Newest durable record:\n");
        print_record(&latest);
        if(!runnable)
        {
            psvDebugScreenPrintf("Existing journal; one-shot probe locked.\n");
            if(latest.hazard_armed != 0)
                psvDebugScreenPrintf("Bit 228 may differ: REBOOT before work.\n");
            psvDebugScreenPrintf("Pull both journal slots before clearing.\n");
        }
    }
    else if(journal_error)
    {
        psvDebugScreenPrintf("Journal conflict/corruption; probe locked.\n");
        psvDebugScreenPrintf("Pull both slots before clearing them.\n");
    }
    else
    {
        psvDebugScreenPrintf("No prior set/restore journal.\n");
    }
    psvDebugScreenPrintf("\nHold L+R and press X once; O exits.\n");

    for(;;)
    {
        uint32_t pressed;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~previous.buttons;
        previous = pad;

        if((pressed & SCE_CTRL_CIRCLE) != 0)
            return 0;
        if((pressed & SCE_CTRL_CROSS) != 0 &&
           (pad.buttons & SCE_CTRL_LTRIGGER) != 0 &&
           (pad.buttons & SCE_CTRL_RTRIGGER) != 0)
        {
            if(ran)
                psvDebugScreenPrintf("Already ran; do not repeat this launch.\n");
            else if(!runnable)
                psvDebugScreenPrintf("Locked: inspect the journal first.\n");
            else
            {
                ran = 1;
                psvDebugScreenPrintf("\nStarting one set/read/restore cycle...\n");
                if(run_probe(&latest, &latest_slot, have_latest) >= 0)
                    psvDebugScreenPrintf("Exact restore verified: PASS.\n");
                else
                    psvDebugScreenPrintf("Probe failed. REBOOT before work.\n");
            }
        }
        sceKernelDelayThread(16000);
    }
}
