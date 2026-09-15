#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/perf.h>
#include <psp2/sysmodule.h>
#include <taihen.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#define DISCOVERY_JOURNAL_PATH \
    "ux0:data/vitaprofiler-pmu-discovery-v1.txt"
#define PERF_MODULE_PATH "vs0:sys/external/libperf.suprx"
#define SCE_PERF_LIBRARY_NID UINT32_C(0x447F047D)
#define RESULT_SECONDS 120u

struct export_probe {
    const char* name;
    uint32_t nid;
};

static const struct export_probe perf_exports[] = {
    {"scePerfArmPmonGetCounterValue", UINT32_C(0x6132A497)},
    {"scePerfArmPmonReset", UINT32_C(0x35151735)},
    {"scePerfArmPmonSelectEvent", UINT32_C(0x63CBEA8B)},
    {"scePerfArmPmonSetCounterValue", UINT32_C(0x12F6C708)},
    {"scePerfArmPmonStart", UINT32_C(0xC9D969D5)},
    {"scePerfArmPmonStop", UINT32_C(0xD1A40F54)},
};

static SceUID journal_fd = -1;
static uint32_t journal_sequence;
static int journal_error;
static int check_count;
static int failure_count;

static int write_all(SceUID fd, const char* text, size_t length)
{
    size_t offset = 0u;

    while (offset < length) {
        SceSSize written =
            sceIoWrite(fd, text + offset, (SceSize)(length - offset));
        if (written < 0)
            return (int)written;
        if (written == 0)
            return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int journal_checkpoint(const char* stage, int code,
                              const char* format, ...)
{
    char message[512];
    char record[768];
    va_list arguments;
    int message_length;
    int record_length;
    int result;

    if (journal_fd < 0)
        return -1;
    va_start(arguments, format);
    message_length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (message_length < 0 || message_length >= (int)sizeof(message)) {
        journal_error = -1;
        return -1;
    }
    record_length = snprintf(record, sizeof(record),
                             "sequence=%u stage=%s code=%d %s\n",
                             ++journal_sequence, stage, code, message);
    if (record_length < 0 || record_length >= (int)sizeof(record)) {
        journal_error = -1;
        return -1;
    }
    result = write_all(journal_fd, record, (size_t)record_length);
    if (result >= 0)
        result = sceIoSyncByFd(journal_fd, 0);
    if (result < 0)
        journal_error = result;
    return result;
}

static void report_check(const char* name, int passed, int code)
{
    ++check_count;
    if (!passed)
        ++failure_count;
    psvDebugScreenPrintf("[%s] %s (code=%d)\n",
                         passed ? "PASS" : "FAIL", name, code);
    (void)journal_checkpoint("check", code, "result=%s name=%s",
                             passed ? "pass" : "fail", name);
}

static int journal_open(void)
{
    static const char header[] =
        "VITAPROFILER-PMU-DISCOVERY-1\n"
        "mode=read-only-loader-and-export-discovery\n"
        "pmu_calls=0\n";
    int result;

    journal_fd = sceIoOpen(DISCOVERY_JOURNAL_PATH,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           0666);
    if (journal_fd < 0) {
        journal_error = journal_fd;
        return journal_fd;
    }
    result = write_all(journal_fd, header, sizeof(header) - 1u);
    if (result >= 0)
        result = sceIoSyncByFd(journal_fd, 0);
    if (result < 0)
        journal_error = result;
    return result;
}

static int journal_close(void)
{
    int result;
    int close_result;

    if (journal_fd < 0)
        return journal_error;
    result = sceIoSyncByFd(journal_fd, 0);
    close_result = sceIoClose(journal_fd);
    journal_fd = -1;
    if (result >= 0 && close_result < 0)
        result = close_result;
    if (result < 0)
        journal_error = result;
    return result;
}

static void snapshot_stub(const char* phase, const char* name,
                          uintptr_t address)
{
    const volatile uint32_t* words =
        (const volatile uint32_t*)(address & ~(uintptr_t)1u);

    (void)journal_checkpoint(
        "stub", 0,
        "phase=%s name=%s address=0x%08X words=%08X,%08X,%08X,%08X",
        phase, name, (uint32_t)address, words[0], words[1], words[2],
        words[3]);
}

static void snapshot_imports(const char* phase)
{
    snapshot_stub(phase, "sceKernelGetThreadId",
                  (uintptr_t)sceKernelGetThreadId);
    snapshot_stub(phase, "scePerfArmPmonGetCounterValue",
                  (uintptr_t)scePerfArmPmonGetCounterValue);
    snapshot_stub(phase, "scePerfArmPmonReset",
                  (uintptr_t)scePerfArmPmonReset);
    snapshot_stub(phase, "scePerfArmPmonSelectEvent",
                  (uintptr_t)scePerfArmPmonSelectEvent);
    snapshot_stub(phase, "scePerfArmPmonSetCounterValue",
                  (uintptr_t)scePerfArmPmonSetCounterValue);
    snapshot_stub(phase, "scePerfArmPmonStart",
                  (uintptr_t)scePerfArmPmonStart);
    snapshot_stub(phase, "scePerfArmPmonStop",
                  (uintptr_t)scePerfArmPmonStop);
}

static int resolve_exports(const char* module_name)
{
    size_t index;
    int resolved = 0;

    for (index = 0u;
         index < sizeof(perf_exports) / sizeof(perf_exports[0]);
         ++index) {
        uintptr_t address = 0u;
        int result = taiGetModuleExportFunc(
            module_name, SCE_PERF_LIBRARY_NID, perf_exports[index].nid,
            &address);
        (void)journal_checkpoint(
            "export", result,
            "module=%s library=0x%08X name=%s nid=0x%08X "
            "address=0x%08X",
            module_name, SCE_PERF_LIBRARY_NID, perf_exports[index].name,
            perf_exports[index].nid, (uint32_t)address);
        if (result >= 0 && address != 0u)
            ++resolved;
    }
    return resolved;
}

static void wait_for_circle_or_timeout(void)
{
    uint32_t tick;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    for (tick = 0u; tick < RESULT_SECONDS * 10u; ++tick) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
            (pad.buttons & SCE_CTRL_CIRCLE) != 0u)
            break;
        sceKernelDelayThread(100000u);
    }
}

int main(void)
{
    SceKernelModuleInfo module_info;
    SceKernelSystemSwVersion software;
    SceUID direct_module_id = -1;
    int sysmodule_result;
    int direct_start_status = 0;
    int module_info_result = -1;
    int resolved_count = 0;
    int unload_result = -1;
    int direct_stop_status = 0;
    int journal_result;

    psvDebugScreenInit();
    psvDebugScreenPrintf("VitaProfiler PMU discovery\n");
    psvDebugScreenPrintf("Read-only loader/export test: ZERO PMU calls.\n\n");

    journal_result = journal_open();
    report_check("synced discovery journal opened",
                 journal_result >= 0, journal_result);

    memset(&software, 0, sizeof(software));
    software.size = sizeof(software);
    journal_result = sceKernelGetSystemSwVersion(&software);
    (void)journal_checkpoint(
        "firmware", journal_result, "version=%s value=0x%08X",
        journal_result >= 0 ? software.versionString : "unavailable",
        journal_result >= 0 ? software.version : 0u);

    snapshot_imports("initial");
    sysmodule_result = sceSysmoduleLoadModule(SCE_SYSMODULE_PERF);
    (void)journal_checkpoint("sysmodule_load", sysmodule_result,
                             "module=0x%08X", SCE_SYSMODULE_PERF);
    snapshot_imports("after_sysmodule");

    if (sysmodule_result < 0) {
        direct_module_id = sceKernelLoadStartModule(
            PERF_MODULE_PATH, 0, NULL, 0, NULL, &direct_start_status);
        (void)journal_checkpoint(
            "direct_load", (int)direct_module_id,
            "path=%s module_id=0x%08X start_status=0x%08X",
            PERF_MODULE_PATH, (uint32_t)direct_module_id,
            (uint32_t)direct_start_status);
        snapshot_imports("after_direct");
    }

    memset(&module_info, 0, sizeof(module_info));
    module_info.size = sizeof(module_info);
    if (direct_module_id >= 0) {
        module_info_result =
            sceKernelGetModuleInfo(direct_module_id, &module_info);
        (void)journal_checkpoint(
            "module_info", module_info_result,
            "module_id=0x%08X name=%s path=%s state=0x%08X "
            "segment0=0x%08X size0=0x%08X",
            (uint32_t)direct_module_id,
            module_info_result >= 0 ? module_info.module_name : "",
            module_info_result >= 0 ? module_info.path : "",
            module_info_result >= 0 ? module_info.state : 0u,
            module_info_result >= 0
                ? (uint32_t)(uintptr_t)module_info.segments[0].vaddr
                : 0u,
            module_info_result >= 0 ? module_info.segments[0].memsz : 0u);
    }

    report_check("direct libperf module loads and starts",
                 direct_module_id >= 0 && direct_start_status >= 0,
                 direct_module_id < 0 ? (int)direct_module_id
                                      : direct_start_status);
    report_check("loaded module identity is readable",
                 module_info_result >= 0 &&
                     module_info.module_name[0] != '\0',
                 module_info_result);

    if (module_info_result >= 0 && module_info.module_name[0] != '\0')
        resolved_count = resolve_exports(module_info.module_name);
    report_check("all six ScePerf exports resolve without calling them",
                 resolved_count ==
                     (int)(sizeof(perf_exports) / sizeof(perf_exports[0])),
                 resolved_count);

    if (direct_module_id >= 0) {
        unload_result = sceKernelStopUnloadModule(
            direct_module_id, 0, NULL, 0, NULL, &direct_stop_status);
        (void)journal_checkpoint(
            "direct_unload", unload_result,
            "module_id=0x%08X stop_status=0x%08X",
            (uint32_t)direct_module_id, (uint32_t)direct_stop_status);
        snapshot_imports("after_unload");
        report_check("direct module unloads cleanly",
                     unload_result >= 0, unload_result);
    } else if (sysmodule_result >= 0) {
        unload_result = sceSysmoduleUnloadModule(SCE_SYSMODULE_PERF);
        (void)journal_checkpoint("sysmodule_unload", unload_result,
                                 "module=0x%08X", SCE_SYSMODULE_PERF);
        report_check("Perf sysmodule unloads cleanly",
                     unload_result >= 0, unload_result);
    }

    (void)journal_checkpoint(
        "summary", failure_count == 0 ? 0 : 1,
        "result=%s checks=%d failures=%d sysmodule=%d "
        "direct_module=0x%08X module_info=%d exports=%d unload=%d "
        "journal_error=%d pmu_calls=0",
        failure_count == 0 ? "pass" : "fail", check_count,
        failure_count, sysmodule_result, (uint32_t)direct_module_id,
        module_info_result, resolved_count, unload_result, journal_error);
    journal_result = journal_close();
    if (journal_result < 0 || journal_error < 0) {
        ++failure_count;
        psvDebugScreenPrintf("[FAIL] final journal sync (code=%d)\n",
                             journal_result < 0 ? journal_result
                                                : journal_error);
    }

    psvDebugScreenPrintf("\nResult: %s (%d checks, %d failures)\n",
                         failure_count == 0 ? "PASS" : "FAIL",
                         check_count, failure_count);
    psvDebugScreenPrintf("No ScePerf function was called.\n");
    psvDebugScreenPrintf("Journal: %s\n", DISCOVERY_JOURNAL_PATH);
    psvDebugScreenPrintf("Press Circle to exit (or wait two minutes).\n");
    wait_for_circle_or_timeout();
    return failure_count == 0 ? 0 : 1;
}
