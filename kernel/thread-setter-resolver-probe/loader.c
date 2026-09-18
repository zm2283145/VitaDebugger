#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "debugScreen.h"
#include "vd_thread_setter_resolver_record.h"

#define VD_SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)

static const char* record_path(int slot)
{
    return slot == 0 ? VD_THREAD_SETTER_RESOLVER_RECORD_A_PATH :
                       VD_THREAD_SETTER_RESOLVER_RECORD_B_PATH;
}

static const char* state_name(uint32_t state)
{
    switch(state)
    {
        case VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED:
            return "loader armed";
        case VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED:
            return "kernel entered";
        case VD_THREAD_SETTER_RESOLVER_STATE_FIRMWARE_RECORDED:
            return "firmware recorded";
        case VD_THREAD_SETTER_RESOLVER_STATE_MODULE_RECORDED:
            return "module recorded";
        case VD_THREAD_SETTER_RESOLVER_STATE_TARGET_RECORDED:
            return "target recorded";
        case VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE:
            return "complete";
        default:
            return "unknown";
    }
}

static const char* result_name(int32_t result)
{
    switch(result)
    {
        case VD_THREAD_SETTER_RESOLVER_OK: return "PASS";
        case VD_THREAD_SETTER_RESOLVER_NOT_RUN: return "not run";
        case VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_LOOKUP:
            return "module lookup failed";
        case VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_NAME:
            return "module identity mismatch";
        case VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_MISSING:
            return "one or more exports missing";
        default: return "unknown";
    }
}

static const char* target_name(uint32_t kind)
{
    switch(kind)
    {
        case VD_THREAD_SETTER_RESOLVER_CORE_GET: return "core get";
        case VD_THREAD_SETTER_RESOLVER_CORE_SET: return "core set";
        case VD_THREAD_SETTER_RESOLVER_VFP_GET: return "VFP get";
        case VD_THREAD_SETTER_RESOLVER_VFP_SET: return "VFP set";
        default: return "unknown";
    }
}

static int read_slot(int slot,
                     struct vd_thread_setter_resolver_record* record)
{
    uint8_t extra;
    SceUID fd = sceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    int result;
    int close_result;

    if(fd < 0)
        return fd;
    result = sceIoRead(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
    {
        const int extra_result = sceIoRead(fd, &extra, sizeof(extra));
        result = extra_result == 0 ? 0 :
            (extra_result < 0 ? extra_result :
                                VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);
    }
    else if(result >= 0)
    {
        result = VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
    }
    close_result = sceIoClose(fd);
    if(result < 0)
        return result;
    if(close_result < 0)
        return close_result;
    return vd_thread_setter_resolver_record_valid(record) ? 0 :
        VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
}

static int slot_presence(int slot, int* present)
{
    SceIoStat stat;
    const int result = sceIoGetstat(record_path(slot), &stat);

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
    *present = 1;
    return result;
}

static int read_latest(struct vd_thread_setter_resolver_record* record,
                       int* slot)
{
    struct vd_thread_setter_resolver_record a;
    struct vd_thread_setter_resolver_record b;
    int present_a = 1;
    int present_b = 1;
    const int presence_a_result = slot_presence(0, &present_a);
    const int presence_b_result = slot_presence(1, &present_b);
    if(presence_a_result < 0 || presence_b_result < 0)
        return VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
    const int valid_a = present_a && read_slot(0, &a) == 0;
    const int valid_b = present_b && read_slot(1, &b) == 0;
    const int selected = vd_thread_setter_resolver_select_latest(
        present_a, valid_a, valid_a ? a.revision : 0,
        present_b, valid_b, valid_b ? b.revision : 0);
    if(selected < 0)
        return selected;
    *record = selected == 0 ? a : b;
    *slot = selected;
    return 0;
}

static int write_slot_verified(
    int slot, struct vd_thread_setter_resolver_record* record)
{
    struct vd_thread_setter_resolver_record verify;
    SceUID fd;
    int result;
    int close_result;

    record->magic = VD_THREAD_SETTER_RESOLVER_MAGIC;
    record->version = VD_THREAD_SETTER_RESOLVER_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_thread_setter_resolver_checksum(record);
    if(!vd_thread_setter_resolver_record_valid(record))
        return -4;

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
                      struct vd_thread_setter_resolver_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void print_record(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    psvDebugScreenPrintf("seq=%u rev=%u state=%s\n", record->sequence,
                         record->revision, state_name(record->state));
    psvDebugScreenPrintf("result=%s (%d) flags=%08X\n",
                         result_name(record->result), record->result,
                         record->flags);
    if(record->state != VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE)
        return;

    psvDebugScreenPrintf("reported fw=%08X query=%08X (spoofable)\n",
                         record->firmware_version,
                         (uint32_t)record->firmware_result);
    psvDebugScreenPrintf("Firmware is metadata, not the resolver gate.\n");
    psvDebugScreenPrintf("module=%s id=%08X nid=%08X\n",
                         record->module_name[0] ? record->module_name : "-",
                         (uint32_t)record->module_id, record->module_nid);
    psvDebugScreenPrintf("lookup=%08X exports=%08X-%08X\n",
                         (uint32_t)record->module_lookup_result,
                         record->exports_start, record->exports_end);
    psvDebugScreenPrintf("resolved=%u of %u (presence only)\n",
                         record->resolved_count,
                         VD_THREAD_SETTER_RESOLVER_TARGET_COUNT);
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_target* target =
            &record->targets[i];
        psvDebugScreenPrintf("%s %08X: r=%08X a=%08X s=%d f=%02X\n",
                             target_name(target->kind), target->nid,
                             (uint32_t)target->lookup_result,
                             target->raw_address, target->segment_index,
                             target->flags);
    }
}

static int expected_completion(
    const struct vd_thread_setter_resolver_record* record,
    const struct vd_thread_setter_resolver_record* attempt)
{
    return record->revision == attempt->revision + 8u &&
           record->sequence == attempt->sequence &&
           record->state == VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE &&
           vd_thread_setter_resolver_record_valid(record);
}

static int run_probe(struct vd_thread_setter_resolver_record* latest,
                     int* latest_slot, int have_latest)
{
    struct vd_thread_setter_resolver_record attempt;
    tai_module_args_t arguments;
    SceUID module;
    int start_result = -1;
    int start_call;
    int lifecycle_ok = 0;
    int result;

    vd_thread_setter_resolver_init_attempt(&attempt);
    attempt.revision = 1;
    attempt.sequence = 1;
    if(have_latest)
    {
        if(latest->revision > UINT32_MAX - 9u ||
           latest->sequence == UINT32_MAX)
            return -10;
        attempt.revision = latest->revision + 1u;
        attempt.sequence = latest->sequence + 1u;
    }
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

    module = taiLoadKernelModule(VD_THREAD_SETTER_RESOLVER_SKPRX_PATH,
                                 0, NULL);
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
    struct vd_thread_setter_resolver_record latest;
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
        latest_result != VD_THREAD_SETTER_RESOLVER_JOURNAL_EMPTY;
    runnable = !journal_error &&
        (!have_latest ||
         latest.state == VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE);

    psvDebugScreenPrintf("VitaDebugger ThreadMgr prerequisite probe\n\n");
    psvDebugScreenPrintf("READ ONLY: checkpointed fixed NID presence.\n");
    psvDebugScreenPrintf("No dynamic call, dereference, code read, or write.\n\n");
    if(have_latest)
    {
        psvDebugScreenPrintf("Newest durable record:\n");
        print_record(&latest);
    }
    else if(journal_error)
    {
        psvDebugScreenPrintf("Journal conflict/corruption; probe locked.\n");
        psvDebugScreenPrintf("Pull both slots before clearing them.\n");
    }
    else
    {
        psvDebugScreenPrintf("No prior resolver journal.\n");
    }
    psvDebugScreenPrintf("\nX records one sample; O exits.\n");

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
                psvDebugScreenPrintf("\nCheckpointing fixed lookups...\n");
                if(run_probe(&latest, &latest_slot, have_latest) >= 0)
                {
                    if(latest.result == VD_THREAD_SETTER_RESOLVER_OK)
                        psvDebugScreenPrintf("Presence gate passed.\n");
                    else
                        psvDebugScreenPrintf("Discovery complete; gate blocked.\n");
                    psvDebugScreenPrintf("Pull both records before analysis.\n");
                }
                else
                {
                    psvDebugScreenPrintf("Probe lifecycle failed; now locked.\n");
                }
            }
        }
        sceKernelDelayThread(16000);
    }
}
