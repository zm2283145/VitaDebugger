#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "debugScreen.h"
#include "vitadebug_kernel.h"

static volatile unsigned int vfp_worker_ready;
static volatile unsigned int vfp_worker_release;
static uint64_t vfp_pattern[VD_KERNEL_VFP_D_REGISTER_COUNT]
    __attribute__((aligned(8)));

// Regression guard for the counterintuitive hardware result: saved ARM core
// state uses entry 1, while this worker's FPSCR was observed in entry 0.
typedef char vd_vfp_fpscr_entry_d32_v1_must_be_zero[
    VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1 == 0u ? 1 : -1];

/*
 * This worker is deliberately self-contained. It publishes readiness only
 * after all D registers and FPSCR hold known values, and restores the
 * callee-saved VFP state plus FPSCR before returning.
 */
__attribute__((naked, noinline))
static void hold_vfp_pattern(
    const uint64_t* pattern __attribute__((unused)),
    volatile unsigned int* ready __attribute__((unused)),
    volatile unsigned int* release __attribute__((unused)))
{
    __asm__ volatile(
        ".syntax unified\n"
        ".fpu neon\n"
        "push {r4, lr}\n"
        "vpush {d8-d15}\n"
        "vmrs r4, fpscr\n"
        "vldmia r0!, {d0-d15}\n"
        "vldmia r0!, {d16-d31}\n"
        "movw r3, #0\n"
        "movt r3, #0x40\n"
        "vmsr fpscr, r3\n"
        "isb\n"
        "dmb ish\n"
        "movs r3, #1\n"
        "str r3, [r1]\n"
        "1:\n"
        "ldr r3, [r2]\n"
        "cmp r3, #0\n"
        "beq 1b\n"
        "vmsr fpscr, r4\n"
        "vpop {d8-d15}\n"
        "pop {r4, pc}\n");
}

static int vfp_worker(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    hold_vfp_pattern(vfp_pattern, &vfp_worker_ready, &vfp_worker_release);
    return 0;
}

static int failures;

static void report_check(const char* name, int passed)
{
    psvDebugScreenPrintf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if(!passed)
        failures++;
}

int main(void)
{
    psvDebugScreenInit();
    SceUID controller_thread = sceKernelGetThreadId();
    uint64_t run_marker = (uint64_t)sceKernelGetSystemTimeWide();
    psvDebugScreenPrintf("VitaDebugger VFP probe: bank-0 diagnostics\n");
    psvDebugScreenPrintf("run=%08X%08X main=%08X\n\n",
                         (unsigned int)(run_marker >> 32),
                         (unsigned int)run_marker,
                         (unsigned int)controller_thread);

    struct vd_kernel_status status = {0};
    int result = vdKernelGetStatus(&status);
    int status_valid = result >= 0 &&
                       status.abi_version == VD_KERNEL_ABI_VERSION;
    report_check("kernel ABI v1.8", status_valid);
    int vfp_supported = status_valid &&
        (status.capabilities & VD_KERNEL_CAP_THREAD_CONTROL) != 0 &&
        (status.capabilities & VD_KERNEL_CAP_THREAD_VFP_REGISTERS) != 0;
    report_check("guarded VFP capability", vfp_supported);

    if(vfp_supported)
    {
        for(unsigned int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
            vfp_pattern[i] = 0xD00D000000000000ULL |
                             ((uint64_t)i << 32) |
                             (uint64_t)(0xA5A50000u + i);

        vfp_worker_ready = 0;
        vfp_worker_release = 0;
        SceUID worker = sceKernelCreateThread(
            "vd VFP pattern worker", vfp_worker, 0x10000100,
            16 * 1024, 0, 0, NULL);
        int worker_started = worker >= 0 &&
            sceKernelStartThread(worker, 0, NULL) >= 0;
        for(int i = 0; worker_started &&
                        !__atomic_load_n(&vfp_worker_ready,
                                         __ATOMIC_SEQ_CST) &&
                        i < 500; ++i)
            sceKernelDelayThread(1000);
        int worker_ready = worker_started &&
            __atomic_load_n(&vfp_worker_ready, __ATOMIC_SEQ_CST);
        report_check("known-pattern worker ready", worker_ready);

        struct vd_kernel_stop_result stop = {0};
        int begin_result = worker_ready
            ? vdKernelBeginStop(1000, -1, &stop)
            : -1;
        report_check("owned stop session", begin_result >= 0 &&
                                           stop.token != 0);

        struct vd_thread_vfp_registers registers = {0};
        int null_rejected = begin_result >= 0 &&
            vdKernelGetThreadVfpRegisters(stop.token, worker, NULL) < 0;
        int wrong_token_rejected = begin_result >= 0 &&
            vdKernelGetThreadVfpRegisters(stop.token ^ 0x80000000u, worker,
                                           &registers) < 0;
        int unowned_rejected = begin_result >= 0 &&
            vdKernelGetThreadVfpRegisters(stop.token,
                                           controller_thread,
                                           &registers) < 0;
        int negative_target_rejected = begin_result >= 0 &&
            vdKernelGetThreadVfpRegisters(stop.token, -1, &registers) < 0;
        int snapshot_result = begin_result >= 0
            ? vdKernelGetThreadVfpRegisters(stop.token, worker, &registers)
            : -1;

        int layout_match = snapshot_result >= 0 &&
            registers.layout_version == VD_KERNEL_VFP_LAYOUT_D32_V1 &&
            registers.d_register_count == VD_KERNEL_VFP_D_REGISTER_COUNT;
        int pattern_match = layout_match;
        if(pattern_match)
            for(unsigned int i = 0;
                i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
                if(registers.d[i] != vfp_pattern[i])
                {
                    pattern_match = 0;
                    break;
                }
        int fpscr_match = snapshot_result >= 0 &&
            registers.fpscr_entry[VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1] ==
                0x00400000u;

        int resumed_count = -1;
        int end_result = begin_result >= 0
            ? vdKernelEndStop(stop.token, &resumed_count)
            : -1;
        __atomic_store_n(&vfp_worker_release, 1, __ATOMIC_SEQ_CST);

        SceUInt wait_timeout = 2000000;
        int worker_status = -1;
        int worker_end_result = worker_started
            ? sceKernelWaitThreadEnd(worker, &worker_status, &wait_timeout)
            : -1;
        if(worker_end_result >= 0)
            sceKernelDeleteThread(worker);
        else if(!worker_started && worker >= 0)
            sceKernelDeleteThread(worker);

        report_check("reject NULL output", null_rejected);
        report_check("reject wrong token", wrong_token_rejected);
        report_check("reject unowned caller thread", unowned_rejected);
        report_check("reject negative target", negative_target_rejected);
        report_check("guarded VFP snapshot", snapshot_result >= 0);
        report_check("D32 layout metadata", layout_match);
        report_check("D0-D31 known-pattern mapping", pattern_match);
        report_check("saved FPSCR bank 0", fpscr_match);
        report_check("end stop session", end_result >= 0 &&
                                         resumed_count >= 1);
        report_check("worker restored and exited", worker_end_result >= 0 &&
                                                   worker_status == 0);

        psvDebugScreenPrintf(
            "\ncalls: begin=%d snapshot=%d end=%d worker_end=%d\n",
            begin_result, snapshot_result, end_result, worker_end_result);
        psvDebugScreenPrintf(
            "stop: token=%08X new=%d already=%d fail=%08X/%d\n",
            stop.token, stop.suspended_count, stop.already_suspended_count,
            (unsigned int)stop.failed_thread, stop.failure_code);
        psvDebugScreenPrintf(
            "finish: resumed=%d worker=%08X status=%d/%08X release=%u\n",
            resumed_count, (unsigned int)worker, worker_status,
            (unsigned int)worker_status,
            __atomic_load_n(&vfp_worker_release, __ATOMIC_SEQ_CST));

        if(snapshot_result >= 0)
            psvDebugScreenPrintf(
                "\nd0=%08X%08X d31=%08X%08X\nfpscr=%08X/%08X\n",
                (unsigned int)(registers.d[0] >> 32),
                (unsigned int)registers.d[0],
                (unsigned int)(registers.d[31] >> 32),
                (unsigned int)registers.d[31],
                registers.fpscr_entry[0], registers.fpscr_entry[1]);
    }

    psvDebugScreenPrintf("\nResult: %s\n", failures ? "FAIL" : "PASS");
    psvDebugScreenPrintf("Leave this screen open and report every FAIL line.\n");
    for(;;)
        sceKernelDelayThread(1000000);
}
