#include <stdint.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "debugScreen.h"
#include "vd_hw_probe_record.h"

#define VD_HW_PROBE_SKPRX_PATH \
    "ux0:app/VDCP00003/module/vd-hw-disabled-probe.skprx"

static int read_record(struct vd_hw_probe_record* record)
{
    SceUID fd = sceIoOpen(VD_HW_PROBE_RESULT_PATH, SCE_O_RDONLY, 0);
    if(fd < 0)
        return fd;
    int result = sceIoRead(fd, record, sizeof(*record));
    int close_result = sceIoClose(fd);
    if(result != (int)sizeof(*record))
        return result < 0 ? result : -1;
    if(close_result < 0)
        return close_result;
    if(record->magic != VD_HW_PROBE_MAGIC ||
       record->version != VD_HW_PROBE_VERSION ||
       record->size != (uint32_t)sizeof(*record) ||
       record->checksum != vd_hw_probe_record_checksum(record))
        return -2;
    return 0;
}

static int invalidate_old_record(void)
{
    struct vd_hw_probe_record invalid = {0};
    SceUID fd = sceIoOpen(VD_HW_PROBE_RESULT_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if(fd < 0)
        return fd;
    int result = sceIoWrite(fd, &invalid, sizeof(invalid));
    if(result == (int)sizeof(invalid))
        result = sceIoSyncByFd(fd, 0);
    else if(result >= 0)
        result = -1;
    int close_result = sceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result >= 0)
        result = sceIoSync("ux0:", 0);
    if(result < 0)
        return result;

    /* A valid old PASS can no longer survive this positive invalidation. */
    struct vd_hw_probe_record verify;
    int verify_result = read_record(&verify);
    return verify_result == -2 ? 0 : -3;
}

static const char* result_name(int result)
{
    switch(result)
    {
        case VD_HW_PROBE_OK: return "PASS";
        case VD_HW_PROBE_NOT_RUN: return "probe did not finish";
        case VD_HW_PROBE_ERROR_WRONG_CORE: return "wrong CPU core";
        case VD_HW_PROBE_ERROR_WRONG_CPU: return "CPU is not Cortex-A9";
        case VD_HW_PROBE_ERROR_NO_COMPARATORS: return "no comparator";
        case VD_HW_PROBE_ERROR_DEBUG_ACTIVE: return "debug already active";
        case VD_HW_PROBE_ERROR_COMPARATOR_BUSY: return "comparator busy";
        case VD_HW_PROBE_ERROR_CODEC: return "test encoding failed";
        case VD_HW_PROBE_ERROR_BREAK_CONTROL_READBACK:
            return "break control write blocked";
        case VD_HW_PROBE_ERROR_BREAK_VALUE_READBACK:
            return "break value write blocked";
        case VD_HW_PROBE_ERROR_WATCH_CONTROL_READBACK:
            return "watch control write blocked";
        case VD_HW_PROBE_ERROR_WATCH_VALUE_READBACK:
            return "watch value write blocked";
        case VD_HW_PROBE_ERROR_BREAK_RESTORE:
            return "breakpoint restore failed";
        case VD_HW_PROBE_ERROR_WATCH_RESTORE:
            return "watchpoint restore failed";
        case VD_HW_PROBE_ERROR_VECTOR_CATCH_ACTIVE:
            return "vector catch already active";
        case VD_HW_PROBE_ERROR_JOURNAL:
            return "durable journal unavailable";
        default: return "unknown result";
    }
}

static void print_record(const struct vd_hw_probe_record* record)
{
    psvDebugScreenPrintf("\nResult: %s (%d)\n", result_name(record->result),
                         record->result);
    psvDebugScreenPrintf("Stage=%u flags=%08X core=%u\n", record->stage,
                         record->flags, record->core_id);
    psvDebugScreenPrintf("MIDR=%08X DIDR=%08X DSCR=%08X\n",
                         record->raw_midr, record->raw_didr,
                         record->raw_dscr);
    psvDebugScreenPrintf("DBGVCR=%08X break=%u watch=%u context=%u\n",
                         record->raw_dbgvcr, record->breakpoint_count,
                         record->watchpoint_count,
                         record->context_breakpoint_count);
    psvDebugScreenPrintf("BCR0 %08X -> %08X -> %08X\n",
                         record->original_bcr0, record->readback_bcr0,
                         record->restored_bcr0);
    psvDebugScreenPrintf("BVR0 %08X -> %08X -> %08X\n",
                         record->original_bvr0, record->readback_bvr0,
                         record->restored_bvr0);
    psvDebugScreenPrintf("WCR0 %08X -> %08X -> %08X\n",
                         record->original_wcr0, record->readback_wcr0,
                         record->restored_wcr0);
    psvDebugScreenPrintf("WVR0 %08X -> %08X -> %08X\n",
                         record->original_wvr0, record->readback_wvr0,
                         record->restored_wvr0);
    psvDebugScreenPrintf("journal initial=%08X final=%08X\n",
                         (uint32_t)record->initial_journal_result,
                         (uint32_t)record->final_journal_result);
}

int main(void)
{
    SceCtrlData previous = {0};
    SceCtrlData pad = {0};

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlPeekBufferPositive(0, &previous, 1);
    psvDebugScreenPrintf("VitaDebugger disabled HW register probe\n\n");
    psvDebugScreenPrintf("This does NOT enable a breakpoint or watchpoint.\n");
    psvDebugScreenPrintf("It writes only E=0 values, reads them back,\n");
    psvDebugScreenPrintf("then restores and verifies the exact snapshot.\n\n");
    psvDebugScreenPrintf("Press X once to run. Press O to exit.\n");

    struct vd_hw_probe_record previous_record;
    if(read_record(&previous_record) >= 0 &&
       previous_record.stage != VD_HW_PROBE_STAGE_COMPLETE)
    {
        psvDebugScreenPrintf("\nPrevious incomplete run found:\n");
        print_record(&previous_record);
        psvDebugScreenPrintf("\nPress X to replace it, or O to exit.\n");
    }

    for(;;)
    {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~previous.buttons;
        previous = pad;
        if((pressed & SCE_CTRL_CIRCLE) != 0)
            return 0;
        if((pressed & SCE_CTRL_CROSS) != 0)
            break;
        sceKernelDelayThread(16000);
    }

    sceIoMkdir("ux0:data/VitaDebugger", 0777);
    int invalidate_result = invalidate_old_record();
    if(invalidate_result < 0)
    {
        psvDebugScreenPrintf("\nCannot invalidate old result: %08X\n",
                             (uint32_t)invalidate_result);
        psvDebugScreenPrintf("Probe aborted before loading kernel code.\n");
        goto wait_to_exit;
    }
    psvDebugScreenPrintf("\nLoading one-shot probe...\n");

    SceUID module = taiLoadKernelModule(VD_HW_PROBE_SKPRX_PATH, 0, NULL);
    psvDebugScreenPrintf("load=%08X\n", (uint32_t)module);
    if(module < 0)
    {
        psvDebugScreenPrintf("Load failed. Unsafe homebrew permission and\n");
        psvDebugScreenPrintf("taiHEN are required.\n");
        goto wait_to_exit;
    }

    tai_module_args_t arguments;
    arguments.size = sizeof(arguments);
    arguments.pid = KERNEL_PID;
    arguments.args = 0;
    arguments.argp = NULL;
    arguments.flags = 0;
    int start_result = -1;
    int start_call = taiStartKernelModuleForUser(module, &arguments, NULL,
                                                  &start_result);
    psvDebugScreenPrintf("start call=%08X module result=%08X\n",
                         (uint32_t)start_call, (uint32_t)start_result);

    if(start_call < 0)
    {
        int unload_result = taiUnloadKernelModule(module, 0, NULL);
        psvDebugScreenPrintf("Start failed; unload=%08X\n",
                             (uint32_t)unload_result);
        if(unload_result < 0)
        {
            psvDebugScreenPrintf("Residency is uncertain. Reboot before retry.\n");
        }
        goto wait_to_exit;
    }

    struct vd_hw_probe_record record;
    int record_result = read_record(&record);
    if(record_result < 0)
    {
        psvDebugScreenPrintf("Result record unavailable/invalid: %08X\n",
                             (uint32_t)record_result);
    }
    else
    {
        print_record(&record);
    }

    if(start_result != SCE_KERNEL_START_NO_RESIDENT)
    {
        int stop_result = -1;
        int cleanup_result = taiStopUnloadKernelModuleForUser(
            module, &arguments, NULL, &stop_result);
        psvDebugScreenPrintf("\nUnexpected residency; cleanup=%08X/%08X\n",
                             (uint32_t)cleanup_result,
                             (uint32_t)stop_result);
        if(cleanup_result < 0)
            psvDebugScreenPrintf("Reboot before running this probe again.\n");
    }
    else
    {
        psvDebugScreenPrintf("\nThe one-shot module is no longer resident.\n");
    }

wait_to_exit:
    psvDebugScreenPrintf("\nPress O to exit.\n");
    for(;;)
    {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~previous.buttons;
        previous = pad;
        if((pressed & SCE_CTRL_CIRCLE) != 0)
            return 0;
        sceKernelDelayThread(16000);
    }
}
