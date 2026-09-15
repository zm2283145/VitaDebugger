#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "debugScreen.h"
#include "vitadebug_kernel.h"

static volatile int keep_worker_running = 1;
static volatile unsigned int worker_ticks;
static volatile unsigned int newcomer_ticks;

static int probe_worker(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    while(keep_worker_running)
    {
        worker_ticks++;
        sceKernelDelayThread(10000);
    }
    return 0;
}

static int newcomer_worker(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    while(keep_worker_running)
    {
        newcomer_ticks++;
        sceKernelDelayThread(10000);
    }
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

    struct vd_kernel_hw_debug_info hw_debug = {0};
    result = vdKernelGetHardwareDebugInfo(&hw_debug);
    report_check("hardware debug discovery", result >= 0);
    report_check("breakpoint comparator count",
                 hw_debug.breakpoint_count > 0 &&
                 hw_debug.breakpoint_count <= 16);
    report_check("watchpoint comparator count",
                 hw_debug.watchpoint_count > 0 &&
                 hw_debug.watchpoint_count <= 16);
    psvDebugScreenPrintf("  didr=%08X break=%u watch=%u context=%u\n",
                         hw_debug.raw_didr, hw_debug.breakpoint_count,
                         hw_debug.watchpoint_count,
                         hw_debug.context_breakpoint_count);

    struct vd_kernel_pmu_info pmu_before = {0};
    struct vd_kernel_pmu_info pmu_after = {0};
    int pmu_before_result = vdKernelGetPmuInfo(&pmu_before);
    int pmu_after_result = -1;
    for(int attempt = 0; attempt < 16; ++attempt)
    {
        pmu_after_result = vdKernelGetPmuInfo(&pmu_after);
        if(pmu_after_result < 0 || pmu_after.cpu_id == pmu_before.cpu_id)
            break;
    }
    report_check("PMU discovery capability",
                 (status.capabilities & VD_KERNEL_CAP_PMU_DISCOVERY) != 0);
    report_check("read-only PMU snapshots",
                 pmu_before_result >= 0 && pmu_after_result >= 0);
    report_check("PMU inventory ABI",
                 pmu_before.struct_size == sizeof(pmu_before) &&
                 pmu_before.abi_version == VD_KERNEL_PMU_ABI_VERSION);
    report_check("Cortex-A9 PMU identity",
                 pmu_before.cpu_id < VD_KERNEL_PMU_PHYSICAL_CORE_COUNT &&
                 (pmu_before.raw_mpidr & 0xffu) == pmu_before.cpu_id &&
                 (pmu_before.raw_midr & 0xff0ffff0u) == 0x410fc090u &&
                 pmu_before.event_counter_count == 6u);
    report_check("same-core PMU comparison",
                 pmu_before_result >= 0 && pmu_after_result >= 0 &&
                 pmu_before.cpu_id == pmu_after.cpu_id);
    report_check("PMU discovery leaves controls unchanged",
                 pmu_before_result >= 0 && pmu_after_result >= 0 &&
                 pmu_before.cpu_id == pmu_after.cpu_id &&
                 pmu_before.raw_pmcr == pmu_after.raw_pmcr &&
                 pmu_before.raw_pmcntenset ==
                     pmu_after.raw_pmcntenset &&
                 pmu_before.raw_pmselr == pmu_after.raw_pmselr &&
                 pmu_before.raw_pmuserenr ==
                     pmu_after.raw_pmuserenr &&
                 pmu_before.raw_pmintenset ==
                     pmu_after.raw_pmintenset);
    psvDebugScreenPrintf(
        "  pmu cpu=%u counters=%u pmcr=%08X enable=%08X user=%08X\n",
        pmu_before.cpu_id, pmu_before.event_counter_count,
        pmu_before.raw_pmcr, pmu_before.raw_pmcntenset,
        pmu_before.raw_pmuserenr);
    psvDebugScreenPrintf(
        "  overflow=%08X select=%08X cycle=%08X irq=%08X\n",
        pmu_before.raw_pmovsr, pmu_before.raw_pmselr,
        pmu_before.raw_pmccntr, pmu_before.raw_pmintenset);

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

    if(worker_started &&
       (status.capabilities & VD_KERNEL_CAP_PROBE_SUSPEND) != 0)
    {
        sceKernelDelayThread(50000);
        unsigned int before = worker_ticks;
        struct vd_kernel_probe_suspend_result suspend_result;
        result = vdKernelProbeSuspendThread(worker, 200000, &suspend_result);
        unsigned int stopped_delta = worker_ticks - before;
        sceKernelDelayThread(100000);
        unsigned int resumed_delta = worker_ticks - before - stopped_delta;

        report_check("probe suspend syscall", result >= 0);
        report_check("worker stayed suspended", stopped_delta <= 2);
        report_check("worker resumed", resumed_delta >= 5);
        report_check("kernel suspend result", suspend_result.suspend_result >= 0);
        report_check("kernel resume result", suspend_result.resume_result >= 0);
        psvDebugScreenPrintf("  states: during=%d after=%d recovery=%d\n",
                             suspend_result.state_while_suspended,
                             suspend_result.state_after_resume,
                             suspend_result.recovery_result);
    }

    if(worker_started &&
       (status.capabilities & VD_KERNEL_CAP_THREAD_CONTROL) != 0)
    {
        struct vd_kernel_stop_result stop_result;
        unsigned int before = worker_ticks;
        result = vdKernelBeginStop(1000, -1, &stop_result);
        sceKernelDelayThread(150000);
        unsigned int stopped_delta = worker_ticks - before;
        struct vd_thread_registers thread_registers;
        int register_result = result >= 0
            ? vdKernelGetThreadRegisters(stop_result.token, worker,
                                         &thread_registers)
            : result;
        if(register_result >= 0)
        {
            for(int bank = 0; bank < 2; ++bank)
                psvDebugScreenPrintf(
                    "  reg%d pc=%08X sp=%08X cpsr=%08X fpscr=%08X\n",
                    bank, thread_registers.entry[bank].pc,
                    thread_registers.entry[bank].sp,
                    thread_registers.entry[bank].cpsr,
                    thread_registers.entry[bank].fpscr);
        }
        int resumed_count = -1;
        int end_result = result >= 0
            ? vdKernelEndStop(stop_result.token, &resumed_count)
            : result;
        sceKernelDelayThread(100000);
        unsigned int resumed_delta = worker_ticks - before - stopped_delta;
        report_check("begin stop session", result >= 0 && stop_result.token != 0);
        report_check("session stopped worker", stopped_delta <= 2);
        report_check("read worker register banks", register_result >= 0);
        report_check("end stop session", end_result >= 0 && resumed_count >= 1);
        report_check("session resumed worker", resumed_delta >= 5);

        before = worker_ticks;
        result = vdKernelBeginStop(250, -1, &stop_result);
        sceKernelDelayThread(450000);
        unsigned int watchdog_delta = worker_ticks - before;
        report_check("begin watchdog session", result >= 0);
        report_check("watchdog auto-resume", watchdog_delta >= 10);

        before = worker_ticks;
        result = vdKernelBeginStop(500, worker, &stop_result);
        sceKernelDelayThread(150000);
        unsigned int exempt_delta = worker_ticks - before;
        end_result = result >= 0
            ? vdKernelEndStop(stop_result.token, &resumed_count)
            : result;
        report_check("begin exempt session", result >= 0);
        report_check("exempt worker kept running", exempt_delta >= 8);
        report_check("end exempt session", end_result >= 0);

        result = vdKernelBeginStop(1000, -1, &stop_result);
        SceUID newcomer = sceKernelCreateThread(
            "vd late worker", newcomer_worker, 0x10000100,
            16 * 1024, 0, 0, NULL);
        int newcomer_started = newcomer >= 0 &&
            sceKernelStartThread(newcomer, 0, NULL) >= 0;
        sceKernelDelayThread(80000);
        unsigned int newcomer_before = newcomer_ticks;
        int renew_result = result >= 0 && newcomer_started
            ? vdKernelRenewStop(stop_result.token, 1000)
            : -1;
        sceKernelDelayThread(150000);
        unsigned int newcomer_stopped_delta = newcomer_ticks - newcomer_before;
        end_result = result >= 0
            ? vdKernelEndStop(stop_result.token, &resumed_count)
            : result;
        sceKernelDelayThread(100000);
        unsigned int newcomer_resumed_delta = newcomer_ticks -
            newcomer_before - newcomer_stopped_delta;
        report_check("late worker started", newcomer_started);
        report_check("renew reconciled session", renew_result >= 0);
        report_check("late worker joined stop", newcomer_stopped_delta <= 2);
        report_check("late worker resumed", newcomer_resumed_delta >= 5);
        report_check("end reconciled session", end_result >= 0);
    }

    psvDebugScreenPrintf("\nLeave this screen open and report any FAIL line.\n");
    for(;;)
        sceKernelDelayThread(1000000);
}
