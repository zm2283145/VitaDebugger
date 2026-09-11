#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "debugScreen.h"
#include "vitadebug_kernel.h"

static volatile int keep_worker_running = 1;

static int probe_worker(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    while(keep_worker_running)
        sceKernelDelayThread(10000);
    return 0;
}

static void report_check(const char* name, int passed)
{
    psvDebugScreenPrintf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
}

int main(void)
{
    psvDebugScreenInit();
    psvDebugScreenPrintf("VitaDebugger kernel boundary probe\n\n");

    SceUID worker = sceKernelCreateThread("vd probe worker", probe_worker,
                                          0x10000100, 16 * 1024, 0, 0, NULL);
    int worker_started = worker >= 0 && sceKernelStartThread(worker, 0, NULL) >= 0;
    report_check("worker started", worker_started);

    struct vd_kernel_status status = {0};
    int result = vdKernelGetStatus(&status);
    report_check("status syscall", result >= 0);
    report_check("ABI version", status.abi_version == VD_KERNEL_ABI_VERSION);
    report_check("thread-list capability",
                 (status.capabilities & VD_KERNEL_CAP_THREAD_LIST) != 0);
    report_check("thread limit", status.max_threads == VD_KERNEL_MAX_THREADS);

    int copied = -1;
    int total = -1;
    result = vdKernelGetThreadList(NULL, 0, &copied, &total);
    report_check("count-only query", result >= 0 && copied == 0 && total >= 1);

    SceUID ids[VD_KERNEL_MAX_THREADS];
    copied = -1;
    total = -1;
    result = vdKernelGetThreadList(ids, VD_KERNEL_MAX_THREADS, &copied, &total);
    report_check("thread enumeration", result >= 0 && copied >= 1 && total >= copied);

    int found_main = 0;
    int found_worker = 0;
    SceUID main_id = sceKernelGetThreadId();
    for(int i = 0; i < copied; ++i)
    {
        if(ids[i] == main_id)
            found_main = 1;
        if(ids[i] == worker)
            found_worker = 1;
        psvDebugScreenPrintf("  thread[%d] = 0x%08X\n", i,
                             (unsigned int)ids[i]);
    }
    report_check("main thread visible", found_main);
    report_check("worker thread visible", !worker_started || found_worker);

    report_check("reject negative capacity",
                 vdKernelGetThreadList(ids, -1, &copied, &total) < 0);
    report_check("reject oversized capacity",
                 vdKernelGetThreadList(ids, VD_KERNEL_MAX_THREADS + 1,
                                       &copied, &total) < 0);
    report_check("reject NULL ids",
                 vdKernelGetThreadList(NULL, 1, &copied, &total) < 0);
    report_check("reject NULL copied count",
                 vdKernelGetThreadList(ids, 1, NULL, &total) < 0);
    report_check("reject NULL total count",
                 vdKernelGetThreadList(ids, 1, &copied, NULL) < 0);

    psvDebugScreenPrintf("\nLeave this screen open and report any FAIL line.\n");
    for(;;)
        sceKernelDelayThread(1000000);
}
