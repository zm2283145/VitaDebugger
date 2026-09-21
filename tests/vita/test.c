#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <psp2/kernel/modulemgr.h>
#ifdef UVDB_STARTUP_DIAGNOSTIC
#include <psp2/kernel/threadmgr/thread.h>
#endif
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

#ifdef UVDB_STARTUP_DIAGNOSTIC
#define UVDB_STARTUP_DIAGNOSTIC_PATH \
    "ux0:/data/vitadebugger-rsp-startup.bin"
#define UVDB_STARTUP_DIAGNOSTIC_MAGIC UINT32_C(0x55565344)
#define UVDB_STARTUP_DIAGNOSTIC_VERSION UINT32_C(5)
#define UVDB_STARTUP_DIAGNOSTIC_FAILED UINT32_C(1)
#define UVDB_STARTUP_OBS_POLL_ENTERED UINT32_C(2)
#define UVDB_STARTUP_OBS_MALFORMED_REQUEST UINT32_C(4)
#define UVDB_STARTUP_OBS_REQUEST_RECEIVED UINT32_C(8)
#define UVDB_STARTUP_OBS_RESPONSE_ATTEMPTED UINT32_C(16)
#define UVDB_STARTUP_OBS_RESPONSE_SENT UINT32_C(32)
#define UVDB_STARTUP_OBS_SELF_PROBE_RECEIVED UINT32_C(64)
#define UVDB_STARTUP_OBS_SELF_PROBE_FAILED UINT32_C(128)
#define UVDB_STARTUP_BUILD_ADMISSION UINT32_C(1)
#define UVDB_STARTUP_BUILD_CONSOLE UINT32_C(2)
#define UVDB_STARTUP_BUILD_KERNEL UINT32_C(4)

enum uvdb_startup_diagnostic_stage
{
    UVDB_STARTUP_STAGE_PROCESS_ENTRY = 1,
    UVDB_STARTUP_STAGE_ROUTE_READY,
    UVDB_STARTUP_STAGE_DISPLAY_READY,
    UVDB_STARTUP_STAGE_KERNEL_GATE,
    UVDB_STARTUP_STAGE_ASLR_GATE,
    UVDB_STARTUP_STAGE_MODULE_ENUM,
    UVDB_STARTUP_STAGE_THREAD_FIXTURE,
    UVDB_STARTUP_STAGE_VFP_FIXTURE,
    UVDB_STARTUP_STAGE_ADMISSION_UDP_BEGIN,
    UVDB_STARTUP_STAGE_ADMISSION_UDP_READY,
    UVDB_STARTUP_STAGE_SERVER_BEGIN,
    UVDB_STARTUP_STAGE_SERVER_READY,
    UVDB_STARTUP_STAGE_STDIO_READY,
    UVDB_STARTUP_STAGE_TEST_READY,
    UVDB_STARTUP_STAGE_MAIN_LOOP,
    UVDB_STARTUP_STAGE_ADMISSION_POLL_BEGIN,
    UVDB_STARTUP_STAGE_ADMISSION_POLL_IDLE,
    UVDB_STARTUP_STAGE_ADMISSION_REQUEST,
    UVDB_STARTUP_STAGE_ADMISSION_RESPONSE,
    UVDB_STARTUP_STAGE_MAIN_LOOP_HEARTBEAT,
};

struct uvdb_startup_diagnostic_record
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t sequence;
    uint32_t stage;
    uint32_t result;
    uint32_t stage_flags;
    uint32_t build_flags;
    uint32_t checksum;
    uint32_t run_id_low;
    uint32_t run_id_high;
    uint32_t failed_stage_mask;
    uint32_t detail[3];
    uint32_t route_address;
    uint32_t bound_address;
    uint32_t bound_port;
    uint32_t endpoint_result;
    uint32_t self_probe_result;
    uint32_t reserved;
};

_Static_assert(
    sizeof(struct uvdb_startup_diagnostic_record) == 84u,
    "startup diagnostic record ABI changed");

static int startup_diagnostic_descriptor = -1;
static uint32_t startup_diagnostic_sequence;
static uint64_t startup_diagnostic_run_id;
static uint32_t startup_diagnostic_failed_stage_mask;
static uint32_t startup_diagnostic_observation_flags;
static uint32_t startup_diagnostic_route_address;
static uint32_t startup_diagnostic_bound_address;
static uint32_t startup_diagnostic_bound_port;
static int startup_diagnostic_endpoint_result;
static int startup_diagnostic_self_probe_result;

static uint32_t startup_diagnostic_checksum(
    const struct uvdb_startup_diagnostic_record* record)
{
    const unsigned char* bytes = (const unsigned char*)record;
    uint32_t hash = UINT32_C(2166136261);
    for(size_t index = 0; index < sizeof(*record); ++index)
    {
        unsigned char value =
            index >= offsetof(struct uvdb_startup_diagnostic_record, checksum) &&
            index < offsetof(struct uvdb_startup_diagnostic_record, checksum) +
                        sizeof(record->checksum)
                ? 0
                : bytes[index];
        hash = (hash ^ value) * UINT32_C(16777619);
    }
    return hash;
}

static uint32_t startup_diagnostic_build_flags(void)
{
    uint32_t flags = 0;
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    flags |= UVDB_STARTUP_BUILD_ADMISSION;
#endif
#ifdef UVDB_GDB_CONSOLE_TEST
    flags |= UVDB_STARTUP_BUILD_CONSOLE;
#endif
#ifdef UVDB_KERNEL_THREAD_CONTROL
    flags |= UVDB_STARTUP_BUILD_KERNEL;
#endif
    return flags;
}

static void startup_diagnostic_mark_detail(
    enum uvdb_startup_diagnostic_stage stage, int result,
    uint32_t stage_flags, uint32_t detail0, uint32_t detail1,
    uint32_t detail2)
{
    if(startup_diagnostic_descriptor < 0)
        return;
    struct uvdb_startup_diagnostic_record record = {
        .magic = UVDB_STARTUP_DIAGNOSTIC_MAGIC,
        .version = UVDB_STARTUP_DIAGNOSTIC_VERSION,
        .size = sizeof(record),
        .sequence = ++startup_diagnostic_sequence,
        .stage = (uint32_t)stage,
        .result = (stage_flags & UVDB_STARTUP_DIAGNOSTIC_FAILED)
                      ? (uint32_t)result
                      : 0u,
        .stage_flags =
            stage_flags | startup_diagnostic_observation_flags,
        .build_flags = startup_diagnostic_build_flags(),
    };
    if(stage_flags & UVDB_STARTUP_DIAGNOSTIC_FAILED)
        startup_diagnostic_failed_stage_mask |=
            UINT32_C(1) << (uint32_t)stage;
    record.run_id_low = (uint32_t)startup_diagnostic_run_id;
    record.run_id_high = (uint32_t)(startup_diagnostic_run_id >> 32);
    record.failed_stage_mask = startup_diagnostic_failed_stage_mask;
    record.detail[0] = detail0;
    record.detail[1] = detail1;
    record.detail[2] = detail2;
    record.route_address = startup_diagnostic_route_address;
    record.bound_address = startup_diagnostic_bound_address;
    record.bound_port = startup_diagnostic_bound_port;
    record.endpoint_result =
        (uint32_t)startup_diagnostic_endpoint_result;
    record.self_probe_result =
        (uint32_t)startup_diagnostic_self_probe_result;
    record.checksum = startup_diagnostic_checksum(&record);
    off_t offset = (off_t)(
        (record.sequence & 1u) * sizeof(record));
    if(lseek(startup_diagnostic_descriptor, offset, SEEK_SET) != offset ||
       write(startup_diagnostic_descriptor, &record, sizeof(record)) !=
           (ssize_t)sizeof(record) ||
       fsync(startup_diagnostic_descriptor) < 0)
    {
        close(startup_diagnostic_descriptor);
        startup_diagnostic_descriptor = -1;
    }
}

static void startup_diagnostic_mark(
    enum uvdb_startup_diagnostic_stage stage, int result,
    uint32_t stage_flags)
{
    startup_diagnostic_mark_detail(stage, result, stage_flags, 0, 0, 0);
}

static void startup_diagnostic_set_route(uint32_t address)
{
    startup_diagnostic_route_address = address;
}

static void startup_diagnostic_set_bound(
    uint32_t address, uint32_t port, int result)
{
    startup_diagnostic_bound_address = address;
    startup_diagnostic_bound_port = port;
    startup_diagnostic_endpoint_result = result;
}

static void startup_diagnostic_set_self_probe(int result)
{
    startup_diagnostic_self_probe_result = result;
    startup_diagnostic_observation_flags |=
        result == 0 ? UVDB_STARTUP_OBS_SELF_PROBE_RECEIVED :
                      UVDB_STARTUP_OBS_SELF_PROBE_FAILED;
}

static void startup_diagnostic_start(void)
{
    startup_diagnostic_sequence = 0;
    startup_diagnostic_run_id = (uint64_t)sceKernelGetSystemTimeWide();
    if(startup_diagnostic_run_id == 0)
        startup_diagnostic_run_id = 1;
    startup_diagnostic_failed_stage_mask = 0;
    startup_diagnostic_observation_flags = 0;
    startup_diagnostic_route_address = 0;
    startup_diagnostic_bound_address = 0;
    startup_diagnostic_bound_port = 0;
    startup_diagnostic_endpoint_result = 0;
    startup_diagnostic_self_probe_result = 0;
    startup_diagnostic_descriptor = open(
        UVDB_STARTUP_DIAGNOSTIC_PATH,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(startup_diagnostic_descriptor < 0)
        return;
    unsigned char empty[2 * sizeof(
        struct uvdb_startup_diagnostic_record)] = {0};
    if(write(startup_diagnostic_descriptor, empty, sizeof(empty)) !=
           (ssize_t)sizeof(empty) ||
       fsync(startup_diagnostic_descriptor) < 0)
    {
        close(startup_diagnostic_descriptor);
        startup_diagnostic_descriptor = -1;
        return;
    }
    startup_diagnostic_mark(UVDB_STARTUP_STAGE_PROCESS_ENTRY, 0, 0);
}
#else
#define startup_diagnostic_start() ((void)0)
#define startup_diagnostic_mark(stage, result, flags) ((void)0)
#define startup_diagnostic_mark_detail(stage, result, flags, d0, d1, d2) \
    ((void)0)
#define startup_diagnostic_set_route(address) ((void)0)
#define startup_diagnostic_set_bound(address, port, result) ((void)0)
#define startup_diagnostic_set_self_probe(result) ((void)0)
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
#ifdef UVDB_STARTUP_DIAGNOSTIC
static uint32_t admission_diagnostic_poll_count;
static int admission_diagnostic_last_receive;
static int admission_diagnostic_last_errno;

static int admission_diagnostic_poll_milestone(uint32_t count)
{
    return count == 1u || count == 10u || count == 50u ||
           count == 100u || count == 150u || count == 200u;
}
#endif

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
#ifdef UVDB_STARTUP_DIAGNOSTIC
    static const char request[] = UVDB_ADMISSION_DIAGNOSTIC_REQUEST;
    struct sockaddr_in bound = {0};
    socklen_t bound_size = sizeof(bound);
    errno = 0;
    int endpoint_result =
        getsockname(descriptor, (void*)&bound, &bound_size);
    startup_diagnostic_set_bound(
        endpoint_result == 0 ? bound.sin_addr.s_addr : 0,
        endpoint_result == 0 ? (uint32_t)ntohs(bound.sin_port) : 0,
        endpoint_result == 0 ? 0 : (errno ? -errno : -1));

    errno = 0;
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    int probe_result = errno ? -errno : -1;
    if(probe >= 0)
    {
        struct sockaddr_in target = {
            .sin_len = sizeof(target),
            .sin_family = AF_INET,
            .sin_addr = {
                .s_addr = startup_diagnostic_route_address != 0
                              ? startup_diagnostic_route_address
                              : htonl(INADDR_LOOPBACK)},
            .sin_port = htons(UVDB_ADMISSION_DIAGNOSTIC_PORT),
        };
        errno = 0;
        int probe_connect_result =
            connect(probe, (void*)&target, sizeof(target));
        probe_result =
            probe_connect_result < 0 ? (errno ? -errno : -1) :
                                       -ETIMEDOUT;
        struct sockaddr_in probe_source = {0};
        socklen_t probe_source_size = sizeof(probe_source);
        int probe_source_result = -1;
        if(probe_connect_result == 0)
        {
            errno = 0;
            probe_source_result =
                getsockname(probe, (void*)&probe_source,
                            &probe_source_size);
            if(probe_source_result < 0)
                probe_result = errno ? -errno : -1;
        }
        errno = 0;
        ssize_t sent =
            probe_source_result == 0
                ? send(probe, request, sizeof(request) - 1u, 0)
                : -1;
        if(probe_source_result == 0)
            probe_result =
                sent < 0 ? (errno ? -errno : -1) :
                sent != (ssize_t)(sizeof(request) - 1u) ? -EIO :
                                                              -ETIMEDOUT;
        for(int wait = 0; sent == (ssize_t)(sizeof(request) - 1u) &&
                             probe_source_result == 0 &&
                             wait < 100; ++wait)
        {
            unsigned char received[sizeof(request)];
            struct sockaddr_in peer = {0};
            socklen_t peer_size = sizeof(peer);
            ssize_t size = recvfrom(
                descriptor, received, sizeof(received),
                MSG_DONTWAIT, (void*)&peer, &peer_size);
            if(size == (ssize_t)(sizeof(request) - 1u) &&
               memcmp(received, request, sizeof(request) - 1u) == 0 &&
               peer.sin_addr.s_addr == probe_source.sin_addr.s_addr &&
               peer.sin_port == probe_source.sin_port)
            {
                probe_result = 0;
                break;
            }
            usleep(1000);
        }
        close(probe);
    }
    startup_diagnostic_set_self_probe(probe_result);
#endif
    admission_diagnostic_socket = descriptor;
    return 0;
}

static void admission_diagnostic_poll(void)
{
    static const char request[] = UVDB_ADMISSION_DIAGNOSTIC_REQUEST;
    unsigned char received[sizeof(request)];
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
#ifdef UVDB_STARTUP_DIAGNOSTIC
    uint32_t poll_count = ++admission_diagnostic_poll_count;
    startup_diagnostic_observation_flags |=
        UVDB_STARTUP_OBS_POLL_ENTERED;
    errno = 0;
#endif
    ssize_t size = recvfrom(
        admission_diagnostic_socket, received, sizeof(received),
        MSG_DONTWAIT, (void*)&peer, &peer_size);
#ifdef UVDB_STARTUP_DIAGNOSTIC
    admission_diagnostic_last_receive = (int)size;
    admission_diagnostic_last_errno = size < 0 ? errno : 0;
#endif
    if(size != (ssize_t)(sizeof(request) - 1u) ||
       memcmp(received, request, sizeof(request) - 1u))
    {
#ifdef UVDB_STARTUP_DIAGNOSTIC
        int first_malformed =
            size >= 0 &&
            !(startup_diagnostic_observation_flags &
              UVDB_STARTUP_OBS_MALFORMED_REQUEST);
        if(first_malformed)
            startup_diagnostic_observation_flags |=
                UVDB_STARTUP_OBS_MALFORMED_REQUEST;
        if(first_malformed ||
           admission_diagnostic_poll_milestone(poll_count))
            startup_diagnostic_mark_detail(
                UVDB_STARTUP_STAGE_ADMISSION_POLL_IDLE, 0, 0,
                (uint32_t)admission_diagnostic_last_receive,
                (uint32_t)admission_diagnostic_last_errno, poll_count);
#endif
        return;
    }

#ifdef UVDB_STARTUP_DIAGNOSTIC
    int first_request =
        !(startup_diagnostic_observation_flags &
          UVDB_STARTUP_OBS_REQUEST_RECEIVED);
    startup_diagnostic_observation_flags |=
        UVDB_STARTUP_OBS_REQUEST_RECEIVED;
    if(first_request)
        startup_diagnostic_mark_detail(
            UVDB_STARTUP_STAGE_ADMISSION_REQUEST, 0, 0,
            (uint32_t)size, (uint32_t)peer_size, poll_count);
#endif
    struct uvdb_admission_diagnostic diagnostic;
    int diagnostic_result = uvdb_admission_diagnostic_get(&diagnostic);
    if(diagnostic_result == 0)
    {
#ifdef UVDB_STARTUP_DIAGNOSTIC
        errno = 0;
#endif
        ssize_t sent = sendto(
            admission_diagnostic_socket, &diagnostic,
            sizeof(diagnostic), 0, (void*)&peer, peer_size);
#ifdef UVDB_STARTUP_DIAGNOSTIC
        int first_response =
            !(startup_diagnostic_observation_flags &
              UVDB_STARTUP_OBS_RESPONSE_ATTEMPTED);
        int first_success =
            sent == (ssize_t)sizeof(diagnostic) &&
            !(startup_diagnostic_observation_flags &
              UVDB_STARTUP_OBS_RESPONSE_SENT);
        startup_diagnostic_observation_flags |=
            UVDB_STARTUP_OBS_RESPONSE_ATTEMPTED;
        if(sent == (ssize_t)sizeof(diagnostic))
            startup_diagnostic_observation_flags |=
                UVDB_STARTUP_OBS_RESPONSE_SENT;
        if(first_response || first_success)
            startup_diagnostic_mark_detail(
                UVDB_STARTUP_STAGE_ADMISSION_RESPONSE,
                sent == (ssize_t)sizeof(diagnostic) ? 0 : (int)sent,
                sent == (ssize_t)sizeof(diagnostic)
                    ? 0
                    : UVDB_STARTUP_DIAGNOSTIC_FAILED,
                (uint32_t)sent, (uint32_t)errno, poll_count);
#else
        (void)sent;
#endif
    }
#ifdef UVDB_STARTUP_DIAGNOSTIC
    else
    {
        int first_response =
            !(startup_diagnostic_observation_flags &
              UVDB_STARTUP_OBS_RESPONSE_ATTEMPTED);
        startup_diagnostic_observation_flags |=
            UVDB_STARTUP_OBS_RESPONSE_ATTEMPTED;
        if(first_response)
            startup_diagnostic_mark_detail(
                UVDB_STARTUP_STAGE_ADMISSION_RESPONSE,
                diagnostic_result, UVDB_STARTUP_DIAGNOSTIC_FAILED,
                (uint32_t)diagnostic_result, 0, poll_count);
    }
#endif
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
    startup_diagnostic_start();
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int route_result = sock;
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(0x08080808)},
        .sin_port = htons(53),
    };
    if(sock >= 0)
    {
        route_result = connect(sock, (void*)&sin, sizeof(sin));
        socklen_t l = sizeof(sin);
        if(route_result == 0)
            route_result = getsockname(sock, (void*)&sin, &l);
        close(sock);
    }
    if(route_result == 0)
        startup_diagnostic_set_route(sin.sin_addr.s_addr);
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_ROUTE_READY, route_result,
        route_result < 0 ? UVDB_STARTUP_DIAGNOSTIC_FAILED : 0);
    psvDebugScreenInit();
    startup_diagnostic_mark(UVDB_STARTUP_STAGE_DISPLAY_READY, 0, 0);
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
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_KERNEL_GATE, kernel_status_result,
        kernel_gate_pass ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
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
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_ASLR_GATE,
        aslr_gate_pass ? 0 : aslr_start_status,
        aslr_gate_pass ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
    if(!aslr_gate_pass)
        hold_failed_gate();
#endif
    SceUID modules[128];
    SceSize module_count = sizeof(modules) / sizeof(modules[0]);
    int module_result = sceKernelGetModuleList(0xff, modules, &module_count);
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_MODULE_ENUM, module_result,
        module_result >= 0 ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
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
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_THREAD_FIXTURE,
        worker_gate_pass ? 0 : -1,
        worker_gate_pass ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
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
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_VFP_FIXTURE,
        vfp_fixture_result == 0 && vfp_fixture_state == 1 ? 0 : -1,
        vfp_fixture_result == 0 && vfp_fixture_state == 1
            ? 0
            : UVDB_STARTUP_DIAGNOSTIC_FAILED);
#endif
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_ADMISSION_UDP_BEGIN, 0, 0);
    int admission_diagnostic_result = admission_diagnostic_start();
    psvDebugScreenPrintf(
        "Admission diagnostic UDP %u: %s (%d)\n",
        UVDB_ADMISSION_DIAGNOSTIC_PORT,
        admission_diagnostic_result == 0 ? "READY" : "FAILED",
        admission_diagnostic_result);
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_ADMISSION_UDP_READY,
        admission_diagnostic_result,
        admission_diagnostic_result == 0
            ? 0
            : UVDB_STARTUP_DIAGNOSTIC_FAILED);
    if(admission_diagnostic_result < 0)
        hold_failed_gate();
#endif
    startup_diagnostic_mark(UVDB_STARTUP_STAGE_SERVER_BEGIN, 0, 0);
    int server_result = uvdb_start_server();
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_SERVER_READY, server_result,
        server_result == 0 ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
    if(server_result < 0)
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
    startup_diagnostic_mark(
        UVDB_STARTUP_STAGE_STDIO_READY, stdio_result,
        stdio_result == 0 ? 0 : UVDB_STARTUP_DIAGNOSTIC_FAILED);
#endif
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_mark_test_ready();
#endif
    startup_diagnostic_mark(UVDB_STARTUP_STAGE_TEST_READY, 0, 0);
    for(int i = 0;; i++)
    {
        if(i == 0)
            startup_diagnostic_mark(
                UVDB_STARTUP_STAGE_MAIN_LOOP, 0, 0);
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        admission_diagnostic_poll();
#ifdef UVDB_STARTUP_DIAGNOSTIC
        if(admission_diagnostic_poll_milestone(
               admission_diagnostic_poll_count))
            startup_diagnostic_mark_detail(
                UVDB_STARTUP_STAGE_MAIN_LOOP_HEARTBEAT, 0, 0,
                admission_diagnostic_poll_count,
                (uint32_t)admission_diagnostic_last_receive,
                ((uint32_t)(uint16_t)admission_diagnostic_socket << 16) |
                    (uint32_t)(uint16_t)admission_diagnostic_last_errno);
#endif
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
