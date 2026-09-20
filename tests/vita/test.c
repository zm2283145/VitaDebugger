#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <psp2/kernel/modulemgr.h>
#include "debugScreen.h"
#include "uvdb.h"
#ifdef UVDB_GDB_CONSOLE_TEST
#include "uvdb_console.h"
#endif
#ifdef UVDB_KERNEL_THREAD_CONTROL
#include "vitadebug_kernel.h"
#endif
#ifdef UVDB_GDB_ASLR_FIXTURE
#include "tests/aslr_fixture/control.h"
#endif

#if defined(UVDB_GDB_VFP_FIXTURE) && !defined(UVDB_KERNEL_VFP_READS)
#error "UVDB_GDB_VFP_FIXTURE requires UVDB_KERNEL_VFP_READS"
#endif

#if defined(UVDB_GDB_ASLR_FIXTURE) && !defined(UVDB_KERNEL_THREAD_CONTROL)
#error "UVDB_GDB_ASLR_FIXTURE requires UVDB_KERNEL_THREAD_CONTROL"
#endif

#ifndef UVDB_DEBUGNET_PORT
#define UVDB_DEBUGNET_PORT 18194
#endif

#ifdef UVDB_ADMISSION_DIAGNOSTIC
#ifndef UVDB_ADMISSION_DIAGNOSTIC_PORT
#define UVDB_ADMISSION_DIAGNOSTIC_PORT 1235
#endif
#define UVDB_ADMISSION_DIAGNOSTIC_REQUEST "UVDB-ADMISSION-1"
static int admission_diagnostic_socket = -1;

static int admission_diagnostic_start(void)
{
    int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if(descriptor < 0)
        return -1;
    int reuse = 1;
    if(setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                  &reuse, sizeof(reuse)) < 0)
    {
        close(descriptor);
        return -1;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = 0},
        .sin_port = htons(UVDB_ADMISSION_DIAGNOSTIC_PORT),
    };
    if(bind(descriptor, (void*)&address, sizeof(address)) < 0)
    {
        close(descriptor);
        return -1;
    }
    admission_diagnostic_socket = descriptor;
    return 0;
}

static void admission_diagnostic_poll(void)
{
    static const char request[] = UVDB_ADMISSION_DIAGNOSTIC_REQUEST;
    unsigned char received[sizeof(request)];
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    ssize_t size = recvfrom(
        admission_diagnostic_socket, received, sizeof(received),
        MSG_DONTWAIT, (void*)&peer, &peer_size);
    if(size != (ssize_t)(sizeof(request) - 1u) ||
       memcmp(received, request, sizeof(request) - 1u))
        return;

    struct uvdb_admission_diagnostic diagnostic;
    if(uvdb_admission_diagnostic_get(&diagnostic) == 0)
        (void)sendto(
            admission_diagnostic_socket, &diagnostic,
            sizeof(diagnostic), 0, (void*)&peer, peer_size);
}
#endif

static volatile int test_value;
volatile int trigger_fault;
volatile uint32_t uvdb_thumb_exclusive_word = UINT32_C(0x13572468);
volatile uint32_t uvdb_arm_exclusive_word = UINT32_C(0x24681357);
static volatile int worker_values[2];
static volatile unsigned int worker_ready_mask;
/*
 * GDB-visible controls for the deterministic foreign-thread step fixture.
 * These are deliberately exported (rather than static) so a test operator can
 * arm and release either worker with ordinary RSP memory writes.  The workers
 * keep their normal counter/sleep behavior while the corresponding arm value
 * is zero.
 */
volatile unsigned int uvdb_worker_step_arm[2];
volatile unsigned int uvdb_worker_step_ready[2];
volatile unsigned int uvdb_worker_step_release[2];
volatile unsigned int uvdb_worker_step_result[2];
#ifdef UVDB_GDB_VFP_FIXTURE
static volatile unsigned int gdb_vfp_fixture_ready;
static volatile unsigned int gdb_vfp_fixture_release;
static uint64_t gdb_vfp_fixture_pattern[32] __attribute__((aligned(8)));
#endif
#ifdef UVDB_GDB_ASLR_FIXTURE
volatile uint32_t uvdb_aslr_main_request;
volatile uint32_t uvdb_aslr_main_acknowledged;
volatile uint32_t uvdb_aslr_main_result;
struct uvdb_aslr_fixture_control uvdb_aslr_fixture_control
    __attribute__((aligned(4)));
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
void thumb_step_exclusive_fixture(void);
void arm_step_exclusive_fixture(void);

__attribute__((noreturn)) static void hold_failed_gate(void)
{
    psvDebugScreenPrintf("Gate failed; debugger not started. Close the app after recording this screen.\n");
    for(;;)
        usleep(1000000);
}

/*
 * Keep these paths as separate, externally visible functions.  Once a ready
 * value is published, worker 0 can execute only path 0 and worker 1 can execute
 * only path 1.  Consequently a temporary breakpoint calculated for one
 * worker's spin loop cannot be reached by the other worker while vCont resumes
 * the process around a selected-thread software step.
 */
__attribute__((noinline, noclone, used))
unsigned int uvdb_worker_step_path_0(unsigned int seed)
{
    uvdb_worker_step_ready[0] = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    while(uvdb_worker_step_release[0] == 0)
        __asm__ volatile("nop" ::: "memory");
    __asm__ volatile("dmb ish" ::: "memory");
    uvdb_worker_step_ready[0] = 0;
    return (seed + 0x101u) ^ 0x13579bdfu;
}

__attribute__((noinline, noclone, used))
unsigned int uvdb_worker_step_path_1(unsigned int seed)
{
    uvdb_worker_step_ready[1] = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    while(uvdb_worker_step_release[1] == 0)
        __asm__ volatile("nop\n\tnop" ::: "memory");
    __asm__ volatile("dmb ish" ::: "memory");
    uvdb_worker_step_ready[1] = 0;
    return (seed ^ 0x2468ace0u) + 0x202u;
}

static void* worker_main(void* argument)
{
    intptr_t index = (intptr_t)argument;
    if(uvdb_register_thread(index == 0 ? "test worker 0" :
                                        "test worker 1") < 0)
        return NULL;
    __atomic_fetch_or(&worker_ready_mask, 1u << (unsigned int)index,
                      __ATOMIC_SEQ_CST);
    for(;;)
    {
        worker_values[index]++;
        if(__atomic_load_n(&uvdb_worker_step_arm[index], __ATOMIC_ACQUIRE))
        {
            unsigned int seed = (unsigned int)worker_values[index];
            unsigned int result = index == 0
                ? uvdb_worker_step_path_0(seed)
                : uvdb_worker_step_path_1(seed);
            __atomic_store_n(&uvdb_worker_step_result[index], result,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&uvdb_worker_step_arm[index], 0,
                             __ATOMIC_RELEASE);
        }
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

#ifdef UVDB_GDB_ASLR_FIXTURE
/* Main-executable half of the live ASLR/source-breakpoint gate. */
__attribute__((noinline, noclone, used, visibility("default")))
uint32_t uvdb_aslr_main_breakpoint(uint32_t sequence)
{
    uint32_t value = sequence ^ UVDB_ASLR_MAIN_RESULT_XOR;
    __asm__ volatile("" : "+r"(value) :: "memory");
    return value;
}
#endif

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
#ifdef UVDB_KERNEL_THREAD_CONTROL
    struct vd_kernel_status kernel_status = {0};
    int kernel_status_result = vdKernelGetStatus(&kernel_status);
    const unsigned int required_capabilities =
        VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES;
    int kernel_gate_pass = kernel_status_result >= 0 &&
        kernel_status.abi_version == VD_KERNEL_ABI_VERSION &&
        (kernel_status.capabilities & required_capabilities) ==
            required_capabilities &&
        kernel_status.max_threads == VD_KERNEL_MAX_THREADS;
    psvDebugScreenPrintf(
        "Kernel thread gate: %s result=%08X ABI=%08X caps=%08X max=%u\n",
        kernel_gate_pass ? "PASS" : "FAIL", kernel_status_result,
        kernel_status.abi_version, kernel_status.capabilities,
        kernel_status.max_threads);
    if(!kernel_gate_pass)
    {
        psvDebugScreenPrintf(
            "Expected ABI=%08X caps&%08X max=%u; debugger not started.\n",
            VD_KERNEL_ABI_VERSION, required_capabilities,
            VD_KERNEL_MAX_THREADS);
        hold_failed_gate();
    }
#endif
#ifdef UVDB_GDB_ASLR_FIXTURE
    memset(&uvdb_aslr_fixture_control, 0,
           sizeof(uvdb_aslr_fixture_control));
    uvdb_aslr_fixture_control.abi_version =
        UVDB_ASLR_FIXTURE_ABI_VERSION;
    uvdb_aslr_fixture_control.size = sizeof(uvdb_aslr_fixture_control);
    uvdb_aslr_main_request = 0;
    uvdb_aslr_main_acknowledged = 0;
    uvdb_aslr_main_result = 0;
    struct uvdb_aslr_fixture_args aslr_fixture_args = {
        .abi_version = UVDB_ASLR_FIXTURE_ABI_VERSION,
        .size = sizeof(aslr_fixture_args),
        .control = &uvdb_aslr_fixture_control,
    };
    int aslr_start_status = SCE_KERNEL_START_FAILED;
    SceUID aslr_module = sceKernelLoadStartModule(
        "app0:/module/uvdb_aslr_fixture.suprx",
        sizeof(aslr_fixture_args), &aslr_fixture_args, 0, NULL,
        &aslr_start_status);
    for(int wait = 0; aslr_module >= 0 &&
                         aslr_start_status == SCE_KERNEL_START_SUCCESS &&
                         __atomic_load_n(&uvdb_aslr_fixture_control.ready,
                                         __ATOMIC_ACQUIRE) == 0 &&
                         wait < 2000; ++wait)
        usleep(1000);
    uint32_t aslr_ready = __atomic_load_n(
        &uvdb_aslr_fixture_control.ready, __ATOMIC_ACQUIRE);
    int aslr_gate_pass = aslr_module >= 0 &&
                         aslr_start_status == SCE_KERNEL_START_SUCCESS &&
                         aslr_ready == UVDB_ASLR_FIXTURE_READY_MAGIC;
    psvDebugScreenPrintf(
        "ASLR SUPRX fixture: %s module=%08X start=%08X ready=%08X\n",
        aslr_gate_pass ? "PASS" : "FAIL", aslr_module,
        aslr_start_status, aslr_ready);
    if(!aslr_gate_pass)
        hold_failed_gate();
#endif
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
    int main_thread_result = uvdb_register_thread("test main");
    pthread_t workers[2];
    worker_ready_mask = 0;
    int worker_result_0 = pthread_create(&workers[0], NULL, worker_main,
                                          (void*)0);
    int worker_result_1 = pthread_create(&workers[1], NULL, worker_main,
                                          (void*)1);
    for(int wait = 0; worker_result_0 == 0 && worker_result_1 == 0 &&
                         __atomic_load_n(&worker_ready_mask,
                                         __ATOMIC_SEQ_CST) != 3u &&
                         wait < 2000; ++wait)
        usleep(1000);
    unsigned int worker_state = __atomic_load_n(&worker_ready_mask,
                                                 __ATOMIC_SEQ_CST);
    int worker_gate_pass = main_thread_result == 0 && worker_result_0 == 0 &&
                           worker_result_1 == 0 && worker_state == 3u;
    psvDebugScreenPrintf(
        "Thread fixture: %s main=%d create=%d,%d ready=%u\n",
        worker_gate_pass ? "PASS" : "FAIL", main_thread_result,
        worker_result_0, worker_result_1, worker_state);
    if(!worker_gate_pass)
        hold_failed_gate();
    psvDebugScreenPrintf(
        "Worker step fixture: ready (distinct paths; arm from GDB)\n");
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
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    int admission_diagnostic_result = admission_diagnostic_start();
    psvDebugScreenPrintf(
        "Admission diagnostic UDP %u: %s (%d)\n",
        UVDB_ADMISSION_DIAGNOSTIC_PORT,
        admission_diagnostic_result == 0 ? "READY" : "FAILED",
        admission_diagnostic_result);
    if(admission_diagnostic_result < 0)
        hold_failed_gate();
#endif
    if(uvdb_start_server() < 0)
    {
        psvDebugScreenPrintf("Failed to start persistent debugger server.\n");
        return 1;
    }
    psvDebugScreenPrintf("Persistent debugger server started.\n");
    psvDebugScreenPrintf("Ctrl-C and clean reconnect are enabled.\n");
#ifdef UVDB_GDB_CONSOLE_TEST
    int stdio_result = uvdb_redirect_stdio();
    psvDebugScreenPrintf("GDB stdout/stderr bridge: %s (%d)\n",
                         stdio_result == 0 ? "READY" : "FAILED",
                         stdio_result);
#endif
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_mark_test_ready();
#endif
    for(int i = 0;; i++)
    {
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        admission_diagnostic_poll();
#endif
#ifdef UVDB_GDB_ASLR_FIXTURE
        uint32_t aslr_main_sequence = __atomic_load_n(
            &uvdb_aslr_main_request, __ATOMIC_ACQUIRE);
        uint32_t aslr_main_done = __atomic_load_n(
            &uvdb_aslr_main_acknowledged, __ATOMIC_ACQUIRE);
        if(aslr_main_sequence != 0 && aslr_main_sequence != aslr_main_done)
        {
            uint32_t aslr_result =
                uvdb_aslr_main_breakpoint(aslr_main_sequence);
            __atomic_store_n(&uvdb_aslr_main_result, aslr_result,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&uvdb_aslr_main_acknowledged,
                             aslr_main_sequence, __ATOMIC_RELEASE);
        }
#endif
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
        thumb_step_exclusive_fixture();
        arm_step_exclusive_fixture();
        if((i % 10) == 0)
        {
            psvDebugScreenPrintf("alive: i=%d value=%d\n", i, test_value);
#ifdef UVDB_GDB_CONSOLE_TEST
            if(stdio_result == 0)
            {
                char marker[96];
                int marker_size = snprintf(
                    marker, sizeof(marker),
                    "[uvdb stdout] tick=%d value=%d\n", i, test_value);
                if(marker_size > 0)
                {
                    size_t output_size = (size_t)marker_size;
                    if(output_size >= sizeof(marker))
                        output_size = sizeof(marker) - 1u;
                    write(STDOUT_FILENO, marker, output_size);
                }
                marker_size = snprintf(
                    marker, sizeof(marker),
                    "[uvdb stderr] workers=%d,%d\n",
                    worker_values[0], worker_values[1]);
                if(marker_size > 0)
                {
                    size_t output_size = (size_t)marker_size;
                    if(output_size >= sizeof(marker))
                        output_size = sizeof(marker) - 1u;
                    write(STDERR_FILENO, marker, output_size);
                }
            }
            if((i % 50) == 0)
            {
                struct uvdb_console_stats console_stats;
                if(uvdb_console_get_stats(&console_stats) == 0)
                    psvDebugScreenPrintf(
                        "console: state=%d session=%u gen=%u q=%u/%u "
                        "sent=%u/%u "
                        "drop=%u+%u+%u+%u\n",
                        (int)uvdb_get_state(),
                        console_stats.session_open,
                        console_stats.session_generation,
                        console_stats.queued_records,
                        console_stats.queued_bytes,
                        console_stats.sent_records,
                        console_stats.sent_bytes,
                        console_stats.dropped_disconnected_bytes,
                        console_stats.dropped_contention_bytes,
                        console_stats.dropped_full_bytes,
                        console_stats.dropped_stale_bytes);
            }
#endif
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
