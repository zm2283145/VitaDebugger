#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <psp2/kernel/modulemgr.h>
#include "debugScreen.h"
#include "uvdb.h"

#if defined(UVDB_GDB_VFP_FIXTURE) && !defined(UVDB_KERNEL_VFP_READS)
#error "UVDB_GDB_VFP_FIXTURE requires UVDB_KERNEL_VFP_READS"
#endif

#ifndef UVDB_DEBUGNET_PORT
#define UVDB_DEBUGNET_PORT 18194
#endif

static volatile int test_value;
volatile int trigger_fault;
static volatile int worker_values[2];
#ifdef UVDB_GDB_VFP_FIXTURE
static volatile unsigned int gdb_vfp_fixture_ready;
static volatile unsigned int gdb_vfp_fixture_release;
static uint64_t gdb_vfp_fixture_pattern[32] __attribute__((aligned(8)));
#endif
#ifdef UVDB_DEBUGNET_LIFECYCLE_TEST
static volatile int debugnet_stress;
#endif

void thumb_step_pop_fixture(void);
void thumb_step_mov_fixture(void);
void thumb_step_tbb_fixture(void);
void thumb_step_tbh_fixture(void);
void thumb_step_ldm_fixture(void);
void thumb_step_it_fixture(void);
void thumb_step_ldmdb_fixture(void);
void thumb_step_ldr_pc_fixture(void);
void arm_step_mov_fixture(void);
void arm_step_ldm_fixture(void);
void arm_step_ldr_pc_fixture(void);

static void* worker_main(void* argument)
{
    intptr_t index = (intptr_t)argument;
    uvdb_register_thread(index == 0 ? "test worker 0" : "test worker 1");
    for(;;)
    {
        worker_values[index]++;
#ifdef UVDB_DEBUGNET_LIFECYCLE_TEST
        if(debugnet_stress)
            uvdb_debugnet_printf(UVDB_LOG_DEBUG,
                                 "worker=%d count=%d\n", (int)index,
                                 worker_values[index]);
#endif
        usleep(20000 + (unsigned int)index * 10000);
    }
    uvdb_unregister_thread();
    return NULL;
}

#ifdef UVDB_GDB_VFP_FIXTURE
/*
 * Keep exact values live without making a function call after publishing
 * readiness. This is intentionally a diagnostic-only busy loop: it gives the
 * kernel all-stop path a foreign, registered thread whose saved VFP bank can
 * be checked through GDB without relying on compiler-generated floating-point
 * code. The function restores the ABI-preserved VFP registers and FPSCR if a
 * future orderly test teardown releases it.
 */
__attribute__((naked, noinline))
static void hold_gdb_vfp_fixture(
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

static void* gdb_vfp_fixture_main(void* argument)
{
    (void)argument;
    if(uvdb_register_thread("GDB VFP fixture") < 0)
    {
        __atomic_store_n(&gdb_vfp_fixture_ready, ~0u, __ATOMIC_SEQ_CST);
        return NULL;
    }
    hold_gdb_vfp_fixture(gdb_vfp_fixture_pattern,
                         &gdb_vfp_fixture_ready,
                         &gdb_vfp_fixture_release);
    uvdb_unregister_thread();
    return NULL;
}
#endif

__attribute__((noinline)) static int step_target(int value)
{
    value += 3;
    if(value & 1)
        value *= 2;
    else
        value -= 1;
    return value;
}

int main(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(0x08080808)},
        .sin_port = htons(53),
    };
    connect(sock, (void*)&sin, sizeof(sin));
    socklen_t l = sizeof(sin);
    getsockname(sock, (void*)&sin, &l);
    close(sock);
    psvDebugScreenInit();
    SceUID modules[128];
    SceSize module_count = sizeof(modules) / sizeof(modules[0]);
    int module_result = sceKernelGetModuleList(0xff, modules, &module_count);
    psvDebugScreenPrintf("modules: result=%08X count=%u\n",
                         module_result, (unsigned int)module_count);
    SceSize displayed_modules = module_count < 3 ? module_count : 3;
    for(SceSize i = 0; module_result >= 0 && i < displayed_modules; ++i)
    {
        SceKernelModuleInfo info = {.size = sizeof(info)};
        int info_result = sceKernelGetModuleInfo(modules[i], &info);
        psvDebugScreenPrintf("  %s info=%08X base=%08X size=%08X\n",
                             info.module_name, info_result,
                             (unsigned int)(uintptr_t)info.segments[0].vaddr,
                             (unsigned int)info.segments[0].memsz);
    }
    uint8_t addr[4];
    memcpy(addr, &sin.sin_addr.s_addr, 4);
    psvDebugScreenPrintf("Run the following command on your PC:\n");
    psvDebugScreenPrintf("$ gdb test.elf -ex 'target remote %hhu.%hhu.%hhu.%hhu:1234'\n", addr[0], addr[1], addr[2], addr[3]);
#ifdef UVDB_DEBUGNET_HOST
    struct uvdb_debugnet_config log_config = {
        .server_ip = UVDB_DEBUGNET_HOST,
        .port = UVDB_DEBUGNET_PORT,
        .level = UVDB_LOG_DEBUG,
    };
    int log_result = uvdb_debugnet_start(&log_config);
    psvDebugScreenPrintf("DebugNet %s -> %s:%u\n",
                         log_result == 0 ? "started" : "failed",
                         UVDB_DEBUGNET_HOST, UVDB_DEBUGNET_PORT);
    uvdb_debugnet_printf(UVDB_LOG_INFO,
                         "VitaDebugger hardware test started (modules=%u)\n",
                         (unsigned int)module_count);
#endif
    uvdb_register_thread("test main");
    pthread_t workers[2];
    pthread_create(&workers[0], NULL, worker_main, (void*)0);
    pthread_create(&workers[1], NULL, worker_main, (void*)1);
#ifdef UVDB_GDB_VFP_FIXTURE
    for(unsigned int i = 0; i < 32; ++i)
        gdb_vfp_fixture_pattern[i] = 0xD00D000000000000ULL |
                                     ((uint64_t)i << 32) |
                                     (uint64_t)(0xA5A50000u + i);
    /* Finite endpoint values make exact GDB console checks unambiguous. */
    gdb_vfp_fixture_pattern[0] = 0x3FF0000000000000ULL;  /* 1.0 */
    gdb_vfp_fixture_pattern[31] = 0x4000000000000000ULL; /* 2.0 */
    gdb_vfp_fixture_ready = 0;
    gdb_vfp_fixture_release = 0;
    pthread_t vfp_fixture;
    int vfp_fixture_result = pthread_create(&vfp_fixture, NULL,
                                             gdb_vfp_fixture_main, NULL);
    for(int wait = 0; vfp_fixture_result == 0 &&
                         __atomic_load_n(&gdb_vfp_fixture_ready,
                                         __ATOMIC_SEQ_CST) == 0 &&
                         wait < 2000; ++wait)
        usleep(1000);
    unsigned int vfp_fixture_state =
        __atomic_load_n(&gdb_vfp_fixture_ready, __ATOMIC_SEQ_CST);
    psvDebugScreenPrintf(
        "GDB VFP fixture: %s (D0=1 D31=2 FPSCR=00400000)\n",
        vfp_fixture_result == 0 && vfp_fixture_state == 1
            ? "ready"
            : "FAILED");
#endif
    if(uvdb_start_server() < 0)
    {
        psvDebugScreenPrintf("Failed to start persistent debugger server.\n");
        return 1;
    }
    psvDebugScreenPrintf("Persistent debugger server started.\n");
    psvDebugScreenPrintf("Ctrl-C and clean reconnect are enabled.\n");
    for(int i = 0;; i++)
    {
#if defined(UVDB_DEBUGNET_HOST) && defined(UVDB_DEBUGNET_LIFECYCLE_TEST)
        if(i == 20)
        {
            int filtered = uvdb_debugnet_printf(UVDB_LOG_TRACE,
                                                "filtered trace\n");
            char oversized[1200];
            memset(oversized, 'T', sizeof(oversized));
            oversized[sizeof(oversized) - 1] = 0;
            int truncated = uvdb_debugnet_write(UVDB_LOG_INFO, oversized);
            debugnet_stress = 1;
            psvDebugScreenPrintf("log edge cases: filtered=%d truncated=%d\n",
                                 filtered, truncated);
        }
        if(i == 100)
        {
            struct uvdb_debugnet_stats before_stop;
            uvdb_debugnet_get_stats(&before_stop);
            int stop_result = uvdb_debugnet_stop();
            int stopped_write = uvdb_debugnet_write(UVDB_LOG_INFO,
                                                     "must not queue");
            int restart_result = uvdb_debugnet_start(&log_config);
            psvDebugScreenPrintf(
                "log restart: stop=%d write=%d start=%d sent=%u drop=%u trunc=%u\n",
                stop_result, stopped_write, restart_result, before_stop.sent,
                before_stop.dropped, before_stop.truncated);
            uvdb_debugnet_printf(UVDB_LOG_INFO,
                                 "debugnet restart completed\n");
        }
        if(i == 110)
            debugnet_stress = 0;
#endif
        if(trigger_fault)
            *(volatile unsigned int*)0 = 0x55464442;
        test_value = step_target(i);
        thumb_step_pop_fixture();
        thumb_step_mov_fixture();
        thumb_step_tbb_fixture();
        thumb_step_tbh_fixture();
        thumb_step_ldm_fixture();
        thumb_step_it_fixture();
        thumb_step_ldmdb_fixture();
        thumb_step_ldr_pc_fixture();
        arm_step_mov_fixture();
        arm_step_ldm_fixture();
        arm_step_ldr_pc_fixture();
        if((i % 10) == 0)
        {
            psvDebugScreenPrintf("alive: i=%d value=%d\n", i, test_value);
#ifdef UVDB_DEBUGNET_HOST
            uvdb_debugnet_printf(UVDB_LOG_DEBUG,
                                 "alive i=%d value=%d workers=%d,%d\n",
                                 i, test_value, worker_values[0], worker_values[1]);
            if((i % 50) == 0)
            {
                struct uvdb_debugnet_stats stats;
                if(uvdb_debugnet_get_stats(&stats) == 0)
                    psvDebugScreenPrintf("logs: sent=%u queued=%u drop=%u trunc=%u err=%u(%d)\n",
                                         stats.sent, stats.queued, stats.dropped,
                                         stats.truncated, stats.send_errors,
                                         stats.last_send_error);
            }
#endif
        }
        usleep(100000);
    }
}
