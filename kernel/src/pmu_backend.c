#include <stddef.h>
#include <stdint.h>

#include "pmu_backend.h"

#if defined(VD_PMU_BACKEND_HOST_TEST)
#include "pmu_backend_host_test.h"

typedef int SceUID;
typedef unsigned int SceUInt;
typedef unsigned int SceSize;
typedef int SceKernelIntrStatus;

#define SCE_EVENT_WAITSINGLE 0
#define SCE_EVENT_WAITOR 0
#define SCE_EVENT_WAITCLEAR 0
#else
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/threadmgr/event_flags.h>
#include <psp2kern/kernel/threadmgr/misc.h>
#include <psp2kern/kernel/threadmgr/thread.h>
#endif

#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION

#define VD_PMU_BACKEND_EVENT_COMMAND 1u
#define VD_PMU_BACKEND_EVENT_STOP 2u
#define VD_PMU_BACKEND_DISPATCH_TIMEOUT_US 250000u
#define VD_PMU_BACKEND_DISPATCH_POLL_US 1000u
#define VD_PMU_BACKEND_LOCK_RETRY_US 1000u

/* PlayStation Vita's Cortex-A9 reports r2p10 on the tested retail hardware. */
#define VD_PMU_BACKEND_CORTEX_A9_MIDR UINT32_C(0x412fc09a)
#define VD_PMU_BACKEND_PMCR_N_SHIFT 11u
#define VD_PMU_BACKEND_PMCR_N_MASK UINT32_C(0x1f)
#define VD_PMU_BACKEND_PMCR_E (UINT32_C(1) << 0)
#define VD_PMU_BACKEND_PMCR_P (UINT32_C(1) << 1)
#define VD_PMU_BACKEND_PMCR_C (UINT32_C(1) << 2)
#define VD_PMU_BACKEND_PMCR_PERSISTENT_MASK UINT32_C(0x39)
#define VD_PMU_BACKEND_PMCR_RESET_MASK \
    (VD_PMU_BACKEND_PMCR_P | VD_PMU_BACKEND_PMCR_C)
#define VD_PMU_BACKEND_EVENT_MASK UINT32_C(0x3f)
#define VD_PMU_BACKEND_CYCLE_MASK (UINT32_C(1) << 31)
#define VD_PMU_BACKEND_IMPLEMENTED_MASK \
    (VD_PMU_BACKEND_EVENT_MASK | VD_PMU_BACKEND_CYCLE_MASK)
#define VD_PMU_BACKEND_COUNTER 5u
#define VD_PMU_BACKEND_COUNTER_MASK \
    (UINT32_C(1) << VD_PMU_BACKEND_COUNTER)
#define VD_PMU_BACKEND_SOFTWARE_EVENT UINT32_C(0x00)
#define VD_PMU_BACKEND_EVENT_TYPE_MASK UINT32_C(0xff)
#define VD_PMU_BACKEND_SELECTOR_MASK UINT32_C(0x1f)

#if defined(VD_PMU_BACKEND_HOST_TEST)
static struct vd_pmu_backend_host_register_ops g_host_register_ops;
static enum vd_pmu_backend_host_dispatch_mode g_host_dispatch_mode;
static uint32_t g_host_current_core;
static uint64_t g_host_time_us;

static int ksceKernelCpuId(void)
{
    return (int)g_host_current_core;
}

static SceKernelIntrStatus ksceKernelCpuSuspendIntr(void)
{
    return 0;
}

static void ksceKernelCpuResumeIntr(SceKernelIntrStatus state)
{
    (void)state;
}

static int ksceKernelDelayThread(SceUInt delay_us)
{
    g_host_time_us += (uint64_t)delay_us;
    return 0;
}

static uint64_t ksceKernelGetSystemTimeWide(void)
{
    return g_host_time_us;
}

static SceUID ksceKernelCreateEventFlag(const char* name, int attributes,
                                        unsigned int initial,
                                        const void* options)
{
    (void)name;
    (void)attributes;
    (void)initial;
    (void)options;
    return -1;
}

static int ksceKernelDeleteEventFlag(SceUID event)
{
    (void)event;
    return 0;
}

static int ksceKernelSetEventFlag(SceUID event, unsigned int bits)
{
    (void)event;
    (void)bits;
    return -1;
}

static int ksceKernelWaitEventFlag(SceUID event, unsigned int requested,
                                   unsigned int wait_mode,
                                   unsigned int* received,
                                   SceUInt* timeout)
{
    (void)event;
    (void)requested;
    (void)wait_mode;
    (void)received;
    (void)timeout;
    return -1;
}

typedef int (*vd_pmu_host_thread_entry)(SceSize args, void* argp);

static SceUID ksceKernelCreateThread(const char* name,
                                     vd_pmu_host_thread_entry entry,
                                     int priority, unsigned int stack_size,
                                     unsigned int attributes, int affinity,
                                     const void* options)
{
    (void)name;
    (void)entry;
    (void)priority;
    (void)stack_size;
    (void)attributes;
    (void)affinity;
    (void)options;
    return -1;
}

static int ksceKernelStartThread(SceUID thread, SceSize args,
                                 const void* argp)
{
    (void)thread;
    (void)args;
    (void)argp;
    return -1;
}

static int ksceKernelWaitThreadEnd(SceUID thread, int* status,
                                   SceUInt* timeout)
{
    (void)thread;
    (void)status;
    (void)timeout;
    return -1;
}

static int ksceKernelDeleteThread(SceUID thread)
{
    (void)thread;
    return 0;
}

static uint32_t host_read_register(
    enum vd_pmu_backend_host_register reg)
{
    if(!g_host_register_ops.read)
        return 0;
    return g_host_register_ops.read(g_host_register_ops.context, reg);
}

static void host_write_register(enum vd_pmu_backend_host_register reg,
                                uint32_t value)
{
    if(g_host_register_ops.write)
        g_host_register_ops.write(g_host_register_ops.context, reg, value);
}

static void host_barrier(enum vd_pmu_backend_host_barrier barrier)
{
    if(g_host_register_ops.barrier)
        g_host_register_ops.barrier(g_host_register_ops.context, barrier);
}
#endif

enum vd_pmu_backend_command_kind {
    VD_PMU_BACKEND_COMMAND_NONE = 0,
    VD_PMU_BACKEND_COMMAND_SNAPSHOT,
    VD_PMU_BACKEND_COMMAND_CONFIGURE,
    VD_PMU_BACKEND_COMMAND_READ,
    VD_PMU_BACKEND_COMMAND_RESTORE,
    VD_PMU_BACKEND_COMMAND_RESTORE_SELECTOR,
    VD_PMU_BACKEND_COMMAND_SELF_TEST,
};

struct vd_pmu_backend_core_state {
    uint32_t raw_mpidr;
    int identity_valid;
    struct vd_pmu_snapshot prepared;
    int prepared_valid;
};

struct vd_pmu_backend_command {
    struct vd_pmu_configuration configuration;
    struct vd_pmu_snapshot original;
    struct vd_pmu_snapshot snapshot_result;
    struct vd_pmu_counter_values values_result;
    struct vd_pmu_backend_test_result test_result;
};

struct vd_pmu_backend_engine {
    SceUID workers[VD_PMU_BACKEND_APP_CORE_COUNT];
    SceUID events[VD_PMU_BACKEND_APP_CORE_COUNT];
    int worker_started[VD_PMU_BACKEND_APP_CORE_COUNT];
    int worker_core[VD_PMU_BACKEND_APP_CORE_COUNT];
    struct vd_pmu_backend_core_state cores[VD_PMU_BACKEND_APP_CORE_COUNT];

    volatile int lock;
    volatile int started;
    volatile int ready;
    volatile int worker_stop;
    volatile int command_inflight;
    volatile int command_complete;
    volatile int command_result;
    volatile unsigned int command_generation;
    volatile enum vd_pmu_backend_command_kind command_kind;
    volatile uint32_t command_core;
    struct vd_pmu_backend_command command;

    int restore_needed;
    int active;
    int saved_valid;
    uint32_t saved_core;
    struct vd_pmu_configuration saved_configuration;
    struct vd_pmu_snapshot saved_snapshot;

    /* A read-only snapshot still has to select lane 5.  Retain this separate
     * record before that first PMSELR write because pmu_session has not yet
     * published its own restoration obligation at snapshot time. */
    int selector_restore_needed;
    uint32_t selector_restore_core;
    uint32_t selector_restore_value;
};

static struct vd_pmu_backend_engine g_pmu;

static void zero_bytes(void* value, size_t size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)value;
    for(size_t i = 0; i < size; ++i)
        bytes[i] = 0;
}

static void copy_bytes(void* destination, const void* source, size_t size)
{
    volatile unsigned char* output =
        (volatile unsigned char*)destination;
    const volatile unsigned char* input =
        (const volatile unsigned char*)source;
    for(size_t i = 0; i < size; ++i)
        output[i] = input[i];
}

static int bytes_equal(const void* left, const void* right, size_t size)
{
    const volatile unsigned char* lhs =
        (const volatile unsigned char*)left;
    const volatile unsigned char* rhs =
        (const volatile unsigned char*)right;
    for(size_t i = 0; i < size; ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static void lock_engine(void)
{
    for(;;)
    {
        int expected = 0;
        if(__atomic_compare_exchange_n(&g_pmu.lock, &expected, 1, 0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST))
            return;
        ksceKernelDelayThread(VD_PMU_BACKEND_LOCK_RETRY_US);
    }
}

static void unlock_engine(void)
{
    __atomic_store_n(&g_pmu.lock, 0, __ATOMIC_SEQ_CST);
}

static void pmu_isb(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_barrier(VD_PMU_BACKEND_HOST_ISB);
#else
    __asm__ volatile("isb" ::: "memory");
#endif
}

static void pmu_dsb(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_barrier(VD_PMU_BACKEND_HOST_DSB);
#else
    __asm__ volatile("dsb" ::: "memory");
#endif
}

static uint32_t read_midr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_MIDR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(value));
    return value;
#endif
}

static uint32_t read_mpidr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_MPIDR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(value));
    return value;
#endif
}

static uint32_t read_pmcr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMCR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(value));
    return value;
#endif
}

static void write_pmcr(uint32_t value)
{
    value &= VD_PMU_BACKEND_PMCR_PERSISTENT_MASK;
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMCR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(value));
#endif
    pmu_isb();
}

static uint32_t read_pmcntenset(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMCNTENSET);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(value));
    return value;
#endif
}

static void write_pmcntenset(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMCNTENSET, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(value));
#endif
    pmu_isb();
}

static void write_pmcntenclr(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMCNTENCLR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" : : "r"(value));
#endif
    pmu_isb();
}

static uint32_t read_pmovsr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMOVSR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(value));
    return value;
#endif
}

static void write_pmovsr(uint32_t value)
{
    /* PMOVSR is write-one-to-clear.  Never replay a saved raw value. */
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMOVSR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 3" : : "r"(value));
#endif
    pmu_isb();
}

static void write_pmswinc(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMSWINC, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 4" : : "r"(value));
#endif
}

static uint32_t read_pmselr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMSELR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 5" : "=r"(value));
    return value;
#endif
}

static void write_pmselr(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMSELR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 5" : : "r"(value));
#endif
    pmu_isb();
}

static uint32_t read_pmccntr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMCCNTR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(value));
    return value;
#endif
}

static uint32_t read_pmxevtyper(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMXEVTYPER);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 1" : "=r"(value));
    return value;
#endif
}

static void write_pmxevtyper(uint32_t value)
{
    value &= VD_PMU_BACKEND_EVENT_TYPE_MASK;
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMXEVTYPER, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 1" : : "r"(value));
#endif
    pmu_isb();
}

static uint32_t read_pmxevcntr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMXEVCNTR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 2" : "=r"(value));
    return value;
#endif
}

static void write_pmxevcntr(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMXEVCNTR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 2" : : "r"(value));
#endif
    pmu_isb();
}

static uint32_t read_pmuserenr(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMUSERENR);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c14, 0" : "=r"(value));
    return value;
#endif
}

static uint32_t read_pmintenset(void)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    return host_read_register(VD_PMU_BACKEND_HOST_PMINTENSET);
#else
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c9, c14, 1" : "=r"(value));
    return value;
#endif
}

static void write_pmintenclr(uint32_t value)
{
#if defined(VD_PMU_BACKEND_HOST_TEST)
    host_write_register(VD_PMU_BACKEND_HOST_PMINTENCLR, value);
#else
    __asm__ volatile("mcr p15, 0, %0, c9, c14, 2" : : "r"(value));
#endif
    pmu_isb();
}

static int verify_core(uint32_t expected_core,
                       uint32_t* raw_midr,
                       uint32_t* raw_mpidr)
{
    if(expected_core >= VD_PMU_BACKEND_APP_CORE_COUNT ||
       (uint32_t)ksceKernelCpuId() != expected_core)
        return VD_PMU_BACKEND_ERROR_CORE;

    const uint32_t midr = read_midr();
    const uint32_t mpidr = read_mpidr();
    if(raw_midr)
        *raw_midr = midr;
    if(raw_mpidr)
        *raw_mpidr = mpidr;
    if(midr != VD_PMU_BACKEND_CORTEX_A9_MIDR ||
       (mpidr & UINT32_C(0xff)) != expected_core)
        return VD_PMU_BACKEND_ERROR_CORE;

    struct vd_pmu_backend_core_state* state = &g_pmu.cores[expected_core];
    if(state->identity_valid)
    {
        if(state->raw_mpidr != mpidr)
            return VD_PMU_BACKEND_ERROR_CORE;
    }
    else
    {
        state->raw_mpidr = mpidr;
        state->identity_valid = 1;
    }
    return 0;
}

static int restore_snapshot_selector(uint32_t core)
{
    if(!g_pmu.selector_restore_needed)
        return 0;
    if(core != g_pmu.selector_restore_core)
        return VD_PMU_BACKEND_ERROR_CORE;
    int result = verify_core(core, NULL, NULL);
    if(result < 0)
        return result;
    write_pmselr(g_pmu.selector_restore_value);
    if(read_pmselr() != g_pmu.selector_restore_value)
        return VD_PMU_BACKEND_ERROR_RESTORE;
    g_pmu.selector_restore_needed = 0;
    return 0;
}

static int capture_snapshot(uint32_t core,
                            struct vd_pmu_snapshot* snapshot,
                            int require_idle_before_selector)
{
    uint32_t unused_midr;
    uint32_t unused_mpidr;
    int result = verify_core(core, &unused_midr, &unused_mpidr);
    if(result < 0 || !snapshot)
        return result < 0 ? result : VD_PMU_BACKEND_ERROR_INVALID;

    struct vd_pmu_snapshot local;
    zero_bytes(&local, sizeof(local));
    local.raw_pmcr = read_pmcr();
    local.event_counter_count =
        (local.raw_pmcr >> VD_PMU_BACKEND_PMCR_N_SHIFT) &
        VD_PMU_BACKEND_PMCR_N_MASK;
    if(local.event_counter_count !=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS ||
       (local.raw_pmcr & VD_PMU_BACKEND_PMCR_RESET_MASK) != 0)
        return VD_PMU_BACKEND_ERROR_CORE;

    local.raw_pmcntenset = read_pmcntenset();
    local.raw_pmovsr = read_pmovsr();
    local.raw_pmselr = read_pmselr();
    local.raw_pmccntr = read_pmccntr();
    local.raw_pmuserenr = read_pmuserenr();
    local.raw_pmintenset = read_pmintenset();
    if((local.raw_pmselr & ~VD_PMU_BACKEND_SELECTOR_MASK) != 0 ||
       (local.raw_pmselr & VD_PMU_BACKEND_SELECTOR_MASK) >=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS)
        return VD_PMU_BACKEND_ERROR_CORE;

    /* The first snapshot has no outer session restore record yet.  Refuse to
     * touch PMSELR if any implemented counter, interrupt, or overflow state
     * indicates another owner. */
    if(require_idle_before_selector &&
       ((local.raw_pmcntenset & VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
        (local.raw_pmintenset & VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
        (local.raw_pmovsr & VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
        local.raw_pmuserenr != 0))
        return VD_PMU_BACKEND_ERROR_NOT_IDLE;

    /* Only lane 5 belongs to this first gate.  Untouched lanes deliberately
     * remain zero in this backend snapshot rather than perturbing PMSELR five
     * extra times merely to inventory state we will never write. */
    if(local.raw_pmselr != VD_PMU_BACKEND_COUNTER)
    {
        g_pmu.selector_restore_core = core;
        g_pmu.selector_restore_value = local.raw_pmselr;
        g_pmu.selector_restore_needed = 1;
        write_pmselr(VD_PMU_BACKEND_COUNTER);
        if(read_pmselr() != VD_PMU_BACKEND_COUNTER)
        {
            result = restore_snapshot_selector(core);
            __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
            return result < 0 ? VD_PMU_BACKEND_ERROR_RESTORE :
                VD_PMU_BACKEND_ERROR_VERIFY;
        }
    }
    local.raw_pmxevtyper[VD_PMU_BACKEND_COUNTER] = read_pmxevtyper();
    local.raw_pmxevcntr[VD_PMU_BACKEND_COUNTER] = read_pmxevcntr();
    if(g_pmu.selector_restore_needed)
    {
        result = restore_snapshot_selector(core);
        if(result < 0)
        {
            __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
            return VD_PMU_BACKEND_ERROR_RESTORE;
        }
    }
    if(read_pmselr() != local.raw_pmselr)
    {
        g_pmu.selector_restore_core = core;
        g_pmu.selector_restore_value = local.raw_pmselr;
        g_pmu.selector_restore_needed = 1;
        result = restore_snapshot_selector(core);
        __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
        return result < 0 ? VD_PMU_BACKEND_ERROR_RESTORE :
            VD_PMU_BACKEND_ERROR_VERIFY;
    }

    copy_bytes(snapshot, &local, sizeof(*snapshot));
    return 0;
}

static int snapshot_equal(const struct vd_pmu_snapshot* left,
                          const struct vd_pmu_snapshot* right)
{
    return left && right && bytes_equal(left, right, sizeof(*left));
}

static int snapshot_is_idle(const struct vd_pmu_snapshot* snapshot)
{
    if(!snapshot ||
       snapshot->event_counter_count !=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS ||
       ((snapshot->raw_pmcr >> VD_PMU_BACKEND_PMCR_N_SHIFT) &
        VD_PMU_BACKEND_PMCR_N_MASK) !=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS ||
       (snapshot->raw_pmcr & VD_PMU_BACKEND_PMCR_RESET_MASK) != 0 ||
       (snapshot->raw_pmcntenset &
        VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
       (snapshot->raw_pmintenset &
        VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
       (snapshot->raw_pmovsr & VD_PMU_BACKEND_IMPLEMENTED_MASK) != 0 ||
       snapshot->raw_pmuserenr != 0 ||
       (snapshot->raw_pmselr & ~VD_PMU_BACKEND_SELECTOR_MASK) != 0 ||
       snapshot->raw_pmselr >=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS)
        return 0;
    for(uint32_t i = 0;
        i < VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS; ++i)
        if((snapshot->raw_pmxevtyper[i] &
            ~VD_PMU_BACKEND_EVENT_TYPE_MASK) != 0)
            return 0;
    return 1;
}

static int backend_event_allowed(
    const struct vd_pmu_event_metadata* event)
{
    if(!event)
        return 0;
    if(event->event_id == VD_PMU_EVENT_SOFTWARE_INCREMENT &&
       event->event_code == VD_PMU_BACKEND_SOFTWARE_EVENT)
        return 1;
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    return (event->event_id == VD_PMU_EVENT_ICACHE_MISS &&
            event->event_code == UINT32_C(0x01)) ||
           (event->event_id == VD_PMU_EVENT_DCACHE_MISS &&
            event->event_code == UINT32_C(0x03)) ||
           (event->event_id == VD_PMU_EVENT_BRANCH_MISPREDICT &&
            event->event_code == UINT32_C(0x10));
#else
    return 0;
#endif
}

static int configuration_valid(
    const struct vd_pmu_configuration* configuration,
    const struct vd_pmu_snapshot* original)
{
    if(!configuration || !original || configuration->flags != 0 ||
       configuration->event_count != 1u || configuration->reserved != 0 ||
       !backend_event_allowed(&configuration->events[0]) ||
       configuration->events[0].physical_counter !=
           VD_PMU_BACKEND_COUNTER ||
       configuration->events[0].reserved != 0)
        return 0;

    const uint32_t expected_control =
        (original->raw_pmcr & VD_PMU_BACKEND_PMCR_E) == 0 ?
        VD_PMU_CONFIGURATION_ENABLE_GLOBAL : 0;
    return configuration->control_flags == expected_control;
}

static int do_snapshot(uint32_t core)
{
    if(g_pmu.restore_needed || g_pmu.selector_restore_needed ||
       g_pmu.active)
        return VD_PMU_BACKEND_ERROR_BUSY;
    int result = capture_snapshot(core, &g_pmu.command.snapshot_result, 1);
    if(result >= 0)
    {
        copy_bytes(&g_pmu.cores[core].prepared,
                   &g_pmu.command.snapshot_result,
                   sizeof(g_pmu.cores[core].prepared));
        g_pmu.cores[core].prepared_valid = 1;
    }
    return result;
}

static int do_configure(uint32_t core,
                        const struct vd_pmu_configuration* configuration)
{
    struct vd_pmu_backend_core_state* state = &g_pmu.cores[core];
    struct vd_pmu_snapshot current;
    struct vd_pmu_snapshot verification;
    int result;

    if(g_pmu.restore_needed || g_pmu.active)
        return VD_PMU_BACKEND_ERROR_BUSY;
    if(!state->prepared_valid)
        return VD_PMU_BACKEND_ERROR_INVALID;

    /* Retain a no-write attempt record too.  pmu_session deliberately calls
     * restore after every configure error because it cannot know whether a
     * backend mutated.  This record lets restore prove that an early reject
     * was a no-op without guessing or touching hardware. */
    copy_bytes(&g_pmu.saved_snapshot, &state->prepared,
               sizeof(g_pmu.saved_snapshot));
    copy_bytes(&g_pmu.saved_configuration, configuration,
               sizeof(g_pmu.saved_configuration));
    g_pmu.saved_core = core;
    g_pmu.saved_valid = 1;
    g_pmu.restore_needed = 0;

    result = capture_snapshot(core, &current, 1);
    if(result < 0)
        return result;
    if(!snapshot_equal(&current, &state->prepared))
        return VD_PMU_BACKEND_ERROR_CONFLICT;
    if(!snapshot_is_idle(&current))
        return VD_PMU_BACKEND_ERROR_NOT_IDLE;
    if(!configuration_valid(configuration, &current))
        return VD_PMU_BACKEND_ERROR_INVALID;

    /* Publish the retained restore record before the first PMU write. */
    copy_bytes(&g_pmu.saved_snapshot, &current,
               sizeof(g_pmu.saved_snapshot));
    g_pmu.restore_needed = 1;
    state->prepared_valid = 0;

    write_pmcntenclr(VD_PMU_BACKEND_COUNTER_MASK);
    write_pmintenclr(VD_PMU_BACKEND_COUNTER_MASK);
    write_pmselr(VD_PMU_BACKEND_COUNTER);
    write_pmxevtyper(configuration->events[0].event_code);
    write_pmxevcntr(0);
    if((read_pmovsr() & VD_PMU_BACKEND_COUNTER_MASK) != 0)
        write_pmovsr(VD_PMU_BACKEND_COUNTER_MASK);
    /* Real events may increment as soon as the lane is enabled.  Prove the
     * selector and zero write while it is still disabled rather than
     * incorrectly requiring a zero value in the later configured snapshot. */
    if((read_pmxevtyper() & VD_PMU_BACKEND_EVENT_TYPE_MASK) !=
           configuration->events[0].event_code ||
       read_pmxevcntr() != 0 ||
       (read_pmovsr() & VD_PMU_BACKEND_COUNTER_MASK) != 0)
        return VD_PMU_BACKEND_ERROR_VERIFY;
    write_pmcntenset(VD_PMU_BACKEND_COUNTER_MASK);
    if((configuration->control_flags &
        VD_PMU_CONFIGURATION_ENABLE_GLOBAL) != 0)
    {
        const uint32_t persistent =
            (current.raw_pmcr & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK) |
            VD_PMU_BACKEND_PMCR_E;
        write_pmcr(persistent);
    }
    write_pmselr(current.raw_pmselr);

    result = capture_snapshot(core, &verification, 0);
    if(result < 0)
        return result;
    struct vd_pmu_snapshot expected = current;
    expected.raw_pmcr =
        (expected.raw_pmcr & ~VD_PMU_BACKEND_PMCR_E) |
        VD_PMU_BACKEND_PMCR_E;
    expected.raw_pmcntenset |= VD_PMU_BACKEND_COUNTER_MASK;
    expected.raw_pmintenset &= ~VD_PMU_BACKEND_COUNTER_MASK;
    expected.raw_pmovsr &= ~VD_PMU_BACKEND_COUNTER_MASK;
    expected.raw_pmxevtyper[VD_PMU_BACKEND_COUNTER] =
        configuration->events[0].event_code;
    if(configuration->events[0].event_id ==
       VD_PMU_EVENT_SOFTWARE_INCREMENT)
        expected.raw_pmxevcntr[VD_PMU_BACKEND_COUNTER] = 0;
    else
        expected.raw_pmxevcntr[VD_PMU_BACKEND_COUNTER] =
            verification.raw_pmxevcntr[VD_PMU_BACKEND_COUNTER];
    if(!snapshot_equal(&verification, &expected))
        return VD_PMU_BACKEND_ERROR_VERIFY;

    g_pmu.active = 1;
    return 0;
}

static int configuration_matches_saved(
    uint32_t core,
    const struct vd_pmu_configuration* configuration,
    const struct vd_pmu_snapshot* original)
{
    return g_pmu.saved_valid && core == g_pmu.saved_core &&
           bytes_equal(configuration, &g_pmu.saved_configuration,
                       sizeof(*configuration)) &&
           snapshot_equal(original, &g_pmu.saved_snapshot);
}

static int do_read(uint32_t core,
                   const struct vd_pmu_configuration* configuration,
                   struct vd_pmu_counter_values* values)
{
    if(!values || !g_pmu.active || !g_pmu.restore_needed ||
       !configuration_matches_saved(core, configuration,
                                    &g_pmu.saved_snapshot))
        return VD_PMU_BACKEND_ERROR_INVALID;
    int result = verify_core(core, NULL, NULL);
    if(result < 0)
        return result;

    const uint32_t enable = read_pmcntenset() &
        VD_PMU_BACKEND_IMPLEMENTED_MASK;
    const uint32_t interrupts = read_pmintenset() &
        VD_PMU_BACKEND_IMPLEMENTED_MASK;
    const uint32_t overflow = read_pmovsr() &
        VD_PMU_BACKEND_IMPLEMENTED_MASK;
    const uint32_t user_enable = read_pmuserenr();
    const uint32_t pmcr = read_pmcr();
    const uint32_t selector = read_pmselr();
    const uint32_t expected_pmcr =
        (g_pmu.saved_snapshot.raw_pmcr &
         VD_PMU_BACKEND_PMCR_PERSISTENT_MASK) |
        VD_PMU_BACKEND_PMCR_E;
    if(enable != VD_PMU_BACKEND_COUNTER_MASK || interrupts != 0 ||
       overflow != 0 ||
       (pmcr & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK) != expected_pmcr ||
       ((pmcr >> VD_PMU_BACKEND_PMCR_N_SHIFT) &
        VD_PMU_BACKEND_PMCR_N_MASK) !=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS ||
       user_enable != g_pmu.saved_snapshot.raw_pmuserenr ||
       selector != g_pmu.saved_snapshot.raw_pmselr)
        return VD_PMU_BACKEND_ERROR_CONFLICT;

    write_pmselr(VD_PMU_BACKEND_COUNTER);
    const uint32_t type = read_pmxevtyper();
    const uint32_t count = read_pmxevcntr();
    write_pmselr(selector);
    if((type & VD_PMU_BACKEND_EVENT_TYPE_MASK) !=
           configuration->events[0].event_code ||
       read_pmselr() != selector)
        return VD_PMU_BACKEND_ERROR_VERIFY;

    zero_bytes(values, sizeof(*values));
    values->events[0] = count;
    return 0;
}

static int do_restore(uint32_t core,
                      const struct vd_pmu_configuration* configuration,
                      const struct vd_pmu_snapshot* original,
                      struct vd_pmu_snapshot* restored)
{
    int result = verify_core(core, NULL, NULL);
    if(result < 0)
        return result;
    if(!configuration || !original || !snapshot_is_idle(original) ||
       !configuration_matches_saved(core, configuration, original))
        return VD_PMU_BACKEND_ERROR_INVALID;

    /* A configure callback may have rejected before its first write.  An
     * idempotent retry after a successful restore also arrives here.  In
     * either case, perform no writes and let pmu_session's following snapshot
     * decide whether the original state still matches exactly. */
    if(!g_pmu.restore_needed)
    {
        struct vd_pmu_snapshot verification;
        if(g_pmu.selector_restore_needed)
        {
            result = restore_snapshot_selector(core);
            if(result < 0)
                return result;
        }
        result = capture_snapshot(core, &verification, 1);
        if(result >= 0 && restored)
            copy_bytes(restored, &verification, sizeof(*restored));
        g_pmu.active = 0;
        return result;
    }
    if(!configuration_valid(configuration, original))
        return VD_PMU_BACKEND_ERROR_INVALID;

    const uint32_t foreign_enable = read_pmcntenset() &
        (VD_PMU_BACKEND_IMPLEMENTED_MASK &
         ~VD_PMU_BACKEND_COUNTER_MASK);
    const uint32_t foreign_interrupt = read_pmintenset() &
        (VD_PMU_BACKEND_IMPLEMENTED_MASK &
         ~VD_PMU_BACKEND_COUNTER_MASK);
    const uint32_t foreign_overflow = read_pmovsr() &
        (VD_PMU_BACKEND_IMPLEMENTED_MASK &
         ~VD_PMU_BACKEND_COUNTER_MASK);
    const uint32_t current_pmcr =
        read_pmcr() & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK;
    const uint32_t original_pmcr =
        original->raw_pmcr & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK;
    const uint32_t current_selector = read_pmselr();
    /* E and PMSELR may legitimately show either side of a partially
     * completed configure.  Every other persistent PMCR bit must still equal
     * the baseline, and PMSELR may only be the baseline or our lane. */
    const int pmcr_conflict =
        (current_pmcr & ~VD_PMU_BACKEND_PMCR_E) !=
            (original_pmcr & ~VD_PMU_BACKEND_PMCR_E) ||
        ((original_pmcr & VD_PMU_BACKEND_PMCR_E) != 0 &&
         (current_pmcr & VD_PMU_BACKEND_PMCR_E) == 0);
    const int selector_conflict =
        current_selector != original->raw_pmselr &&
        current_selector != VD_PMU_BACKEND_COUNTER;
    const int external_conflict =
        foreign_enable != 0 || foreign_interrupt != 0 ||
        foreign_overflow != 0 ||
        read_pmuserenr() != original->raw_pmuserenr ||
        pmcr_conflict || selector_conflict;

    /* Always remove our lane first, even if a foreign owner appeared. */
    write_pmcntenclr(VD_PMU_BACKEND_COUNTER_MASK);
    write_pmintenclr(VD_PMU_BACKEND_COUNTER_MASK);
    write_pmselr(VD_PMU_BACKEND_COUNTER);
    write_pmxevtyper(
        original->raw_pmxevtyper[VD_PMU_BACKEND_COUNTER]);
    write_pmxevcntr(
        original->raw_pmxevcntr[VD_PMU_BACKEND_COUNTER]);
    if((read_pmovsr() & VD_PMU_BACKEND_COUNTER_MASK) != 0)
        write_pmovsr(VD_PMU_BACKEND_COUNTER_MASK);
    write_pmselr(original->raw_pmselr);

    if(external_conflict)
        return VD_PMU_BACKEND_ERROR_CONFLICT;

    const uint32_t desired_pmcr =
        original->raw_pmcr & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK;
    if((read_pmcr() & VD_PMU_BACKEND_PMCR_PERSISTENT_MASK) != desired_pmcr)
        write_pmcr(desired_pmcr);

    struct vd_pmu_snapshot verification;
    result = capture_snapshot(core, &verification, 1);
    if(result < 0)
        return result;
    if(restored)
        copy_bytes(restored, &verification, sizeof(*restored));
    if(!snapshot_equal(&verification, original))
        return VD_PMU_BACKEND_ERROR_RESTORE;

    g_pmu.active = 0;
    g_pmu.restore_needed = 0;
    copy_bytes(&g_pmu.cores[core].prepared, &verification,
               sizeof(g_pmu.cores[core].prepared));
    g_pmu.cores[core].prepared_valid = 1;
    return 0;
}

static int do_self_test(uint32_t core)
{
    struct vd_pmu_backend_test_result* test =
        &g_pmu.command.test_result;
    struct vd_pmu_configuration configuration;
    struct vd_pmu_counter_values values;
    int operation_result = 0;
    int restore_result = 0;
    uint32_t raw_midr = 0;
    uint32_t raw_mpidr = 0;

    zero_bytes(test, sizeof(*test));
    test->struct_size = sizeof(*test);
    test->abi_version = VD_PMU_BACKEND_ABI_VERSION;
    test->core_id = core;
    test->increment_count = VD_PMU_BACKEND_TEST_INCREMENT_COUNT;
    test->stage = VD_PMU_BACKEND_TEST_SNAPSHOT;

    if(g_pmu.active || g_pmu.restore_needed ||
       g_pmu.selector_restore_needed)
    {
        operation_result = VD_PMU_BACKEND_ERROR_BUSY;
        goto done;
    }
    operation_result = verify_core(core, &raw_midr, &raw_mpidr);
    test->raw_midr = raw_midr;
    test->raw_mpidr = raw_mpidr;
    if(operation_result < 0)
        goto done;
    operation_result = capture_snapshot(core, &test->before, 1);
    if(operation_result < 0)
    {
        if(g_pmu.selector_restore_needed)
        {
            test->stage = VD_PMU_BACKEND_TEST_RESTORE;
            restore_result = restore_snapshot_selector(core);
        }
        goto done;
    }
    if(!snapshot_is_idle(&test->before))
    {
        operation_result = VD_PMU_BACKEND_ERROR_NOT_IDLE;
        goto done;
    }
    copy_bytes(&g_pmu.cores[core].prepared, &test->before,
               sizeof(g_pmu.cores[core].prepared));
    g_pmu.cores[core].prepared_valid = 1;

    zero_bytes(&configuration, sizeof(configuration));
    configuration.event_count = 1;
    configuration.control_flags =
        (test->before.raw_pmcr & VD_PMU_BACKEND_PMCR_E) == 0 ?
        VD_PMU_CONFIGURATION_ENABLE_GLOBAL : 0;
    configuration.events[0].event_id =
        VD_PMU_EVENT_SOFTWARE_INCREMENT;
    configuration.events[0].event_code =
        VD_PMU_BACKEND_SOFTWARE_EVENT;
    configuration.events[0].physical_counter = VD_PMU_BACKEND_COUNTER;

    test->stage = VD_PMU_BACKEND_TEST_CONFIGURE;
    operation_result = do_configure(core, &configuration);
    if(operation_result < 0)
        goto restore;

    test->stage = VD_PMU_BACKEND_TEST_INCREMENT;
    pmu_dsb();
    for(uint32_t i = 0; i < VD_PMU_BACKEND_TEST_INCREMENT_COUNT; ++i)
        write_pmswinc(VD_PMU_BACKEND_COUNTER_MASK);
    pmu_isb();

    test->stage = VD_PMU_BACKEND_TEST_READ;
    operation_result = do_read(core, &configuration, &values);
    if(operation_result >= 0)
    {
        test->observed_count = values.events[0];
        if(test->observed_count != VD_PMU_BACKEND_TEST_INCREMENT_COUNT)
            operation_result = VD_PMU_BACKEND_ERROR_VERIFY;
    }

restore:
    if(g_pmu.selector_restore_needed)
    {
        test->stage = VD_PMU_BACKEND_TEST_RESTORE;
        restore_result = restore_snapshot_selector(core);
    }
    if(restore_result >= 0 && g_pmu.restore_needed)
    {
        test->stage = VD_PMU_BACKEND_TEST_RESTORE;
        restore_result = do_restore(core, &configuration, &test->before,
                                    &test->after);
    }
    else if(restore_result >= 0)
    {
        restore_result = capture_snapshot(core, &test->after, 1);
    }
    if(restore_result >= 0 && operation_result >= 0)
    {
        test->stage = VD_PMU_BACKEND_TEST_VERIFY;
        if(!snapshot_equal(&test->after, &test->before))
            operation_result = VD_PMU_BACKEND_ERROR_VERIFY;
    }

done:
    test->operation_result = operation_result;
    test->restore_result = restore_result;
    if(operation_result >= 0 && restore_result >= 0)
        test->stage = VD_PMU_BACKEND_TEST_COMPLETE;
    return restore_result < 0 ? VD_PMU_BACKEND_ERROR_RESTORE :
        operation_result;
}

static int execute_command(uint32_t core,
                           enum vd_pmu_backend_command_kind kind)
{
    SceKernelIntrStatus intr_state = ksceKernelCpuSuspendIntr();
    int result;

    /* Suspending local interrupts keeps the fixed-affinity worker from being
     * preempted in any PMSELR/PMXEV* indexed sequence. */
    if(core >= VD_PMU_BACKEND_APP_CORE_COUNT ||
       (uint32_t)ksceKernelCpuId() != core)
    {
        result = VD_PMU_BACKEND_ERROR_CORE;
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_SNAPSHOT)
    {
        result = do_snapshot(core);
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_CONFIGURE)
    {
        result = do_configure(core, &g_pmu.command.configuration);
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_READ)
    {
        result = do_read(core, &g_pmu.command.configuration,
                         &g_pmu.command.values_result);
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_RESTORE)
    {
        result = do_restore(core, &g_pmu.command.configuration,
                            &g_pmu.command.original,
                            &g_pmu.command.snapshot_result);
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_RESTORE_SELECTOR)
    {
        result = restore_snapshot_selector(core);
    }
    else if(kind == VD_PMU_BACKEND_COMMAND_SELF_TEST)
    {
        result = do_self_test(core);
    }
    else
    {
        result = VD_PMU_BACKEND_ERROR_INVALID;
    }
    ksceKernelCpuResumeIntr(intr_state);
    return result;
}

static int worker_main(SceSize args, void* argp)
{
    if(args != sizeof(int) || !argp)
        return VD_PMU_BACKEND_ERROR_CORE;
    const uint32_t core = (uint32_t)*(int*)argp;
    if(core >= VD_PMU_BACKEND_APP_CORE_COUNT)
        return VD_PMU_BACKEND_ERROR_CORE;

    unsigned int seen = 0;
    for(;;)
    {
        unsigned int events;
        int result = ksceKernelWaitEventFlag(
            g_pmu.events[core],
            VD_PMU_BACKEND_EVENT_COMMAND | VD_PMU_BACKEND_EVENT_STOP,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &events, NULL);
        if(result < 0)
            return result;
        if((events & VD_PMU_BACKEND_EVENT_STOP) != 0 ||
           __atomic_load_n(&g_pmu.worker_stop, __ATOMIC_SEQ_CST))
            break;

        const unsigned int generation =
            __atomic_load_n(&g_pmu.command_generation, __ATOMIC_ACQUIRE);
        const uint32_t target =
            __atomic_load_n(&g_pmu.command_core, __ATOMIC_ACQUIRE);
        if(generation != seen && target == core)
        {
            const enum vd_pmu_backend_command_kind kind =
                __atomic_load_n(&g_pmu.command_kind, __ATOMIC_ACQUIRE);
            result = execute_command(core, kind);
            __atomic_store_n(&g_pmu.command_result, result,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&g_pmu.command_complete, 1,
                             __ATOMIC_RELEASE);
            seen = generation;
        }
    }
    return 0;
}

static int finish_inflight_locked(void)
{
    if(!__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
        return 0;

    const uint64_t deadline =
        (uint64_t)ksceKernelGetSystemTimeWide() +
        VD_PMU_BACKEND_DISPATCH_TIMEOUT_US;
    while(!__atomic_load_n(&g_pmu.command_complete, __ATOMIC_ACQUIRE))
    {
        if((uint64_t)ksceKernelGetSystemTimeWide() >= deadline)
        {
            __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
            return VD_PMU_BACKEND_ERROR_TIMEOUT;
        }
        ksceKernelDelayThread(VD_PMU_BACKEND_DISPATCH_POLL_US);
    }
    const int result =
        __atomic_load_n(&g_pmu.command_result, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_pmu.command_inflight, 0, __ATOMIC_RELEASE);
    return result;
}

static int dispatch_locked(enum vd_pmu_backend_command_kind kind,
                           uint32_t core,
                           int permit_recovery)
{
    if(!g_pmu.started || core >= VD_PMU_BACKEND_APP_CORE_COUNT ||
       (!g_pmu.ready && !permit_recovery))
        return VD_PMU_BACKEND_ERROR_DISABLED;
    if(__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
    {
        int prior = finish_inflight_locked();
        if(prior == VD_PMU_BACKEND_ERROR_TIMEOUT &&
           __atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
            return prior;
    }

    unsigned int generation =
        __atomic_load_n(&g_pmu.command_generation,
                        __ATOMIC_RELAXED) + 1u;
    if(generation == 0)
        generation = 1;
    __atomic_store_n(&g_pmu.command_complete, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pmu.command_result, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pmu.command_kind, kind, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pmu.command_core, core, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pmu.command_inflight, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pmu.command_generation, generation,
                     __ATOMIC_RELEASE);
#if defined(VD_PMU_BACKEND_HOST_TEST)
    if(g_host_dispatch_mode == VD_PMU_BACKEND_HOST_DISPATCH_INLINE)
    {
        g_host_current_core = core;
        const int result = execute_command(core, kind);
        __atomic_store_n(&g_pmu.command_result, result,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_pmu.command_complete, 1,
                         __ATOMIC_RELEASE);
    }
    else if(g_host_dispatch_mode ==
            VD_PMU_BACKEND_HOST_DISPATCH_SIGNAL_FAILURE)
    {
        __atomic_store_n(&g_pmu.command_result,
                         VD_PMU_BACKEND_ERROR_CORE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_pmu.command_complete, 1,
                         __ATOMIC_RELEASE);
    }
#else
    if(ksceKernelSetEventFlag(g_pmu.events[core],
                              VD_PMU_BACKEND_EVENT_COMMAND) < 0)
    {
        __atomic_store_n(&g_pmu.command_result,
                         VD_PMU_BACKEND_ERROR_CORE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_pmu.command_complete, 1,
                         __ATOMIC_RELEASE);
    }
#endif
    return finish_inflight_locked();
}

static int callback_snapshot(void* context, uint32_t core,
                             struct vd_pmu_snapshot* snapshot)
{
    if(context != &g_pmu || !snapshot)
        return VD_PMU_BACKEND_ERROR_INVALID;
    lock_engine();
    int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_SNAPSHOT,
                                 core, 0);
    if(result >= 0)
        copy_bytes(snapshot, &g_pmu.command.snapshot_result,
                   sizeof(*snapshot));
    unlock_engine();
    return result;
}

static int callback_configure(
    void* context, uint32_t core,
    const struct vd_pmu_configuration* configuration)
{
    if(context != &g_pmu || !configuration)
        return VD_PMU_BACKEND_ERROR_INVALID;
    lock_engine();
    copy_bytes(&g_pmu.command.configuration, configuration,
               sizeof(g_pmu.command.configuration));
    /* Publish a provable no-write record before signaling the worker.  If
     * event delivery itself fails, pmu_session still invokes restore because
     * configure has an ambiguous mutation contract.  Matching the prepared
     * snapshot here lets that restore verify a no-op instead of becoming a
     * permanent false restoration obligation. */
    if(core < VD_PMU_BACKEND_APP_CORE_COUNT && !g_pmu.restore_needed &&
       !g_pmu.selector_restore_needed && !g_pmu.active &&
       g_pmu.cores[core].prepared_valid)
    {
        copy_bytes(&g_pmu.saved_snapshot, &g_pmu.cores[core].prepared,
                   sizeof(g_pmu.saved_snapshot));
        copy_bytes(&g_pmu.saved_configuration, configuration,
                   sizeof(g_pmu.saved_configuration));
        g_pmu.saved_core = core;
        g_pmu.saved_valid = 1;
    }
    int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_CONFIGURE,
                                 core, 0);
    unlock_engine();
    return result;
}

static int callback_read(void* context, uint32_t core,
                         const struct vd_pmu_configuration* configuration,
                         struct vd_pmu_counter_values* values)
{
    if(context != &g_pmu || !configuration || !values)
        return VD_PMU_BACKEND_ERROR_INVALID;
    lock_engine();
    copy_bytes(&g_pmu.command.configuration, configuration,
               sizeof(g_pmu.command.configuration));
    int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_READ, core, 0);
    if(result >= 0)
        copy_bytes(values, &g_pmu.command.values_result,
                   sizeof(*values));
    unlock_engine();
    return result;
}

static int callback_restore(void* context, uint32_t core,
                            const struct vd_pmu_configuration* configuration,
                            const struct vd_pmu_snapshot* original)
{
    if(context != &g_pmu || !configuration || !original)
        return VD_PMU_BACKEND_ERROR_INVALID;
    lock_engine();
    copy_bytes(&g_pmu.command.configuration, configuration,
               sizeof(g_pmu.command.configuration));
    copy_bytes(&g_pmu.command.original, original,
               sizeof(g_pmu.command.original));
    int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_RESTORE,
                                 core, 1);
    /* A timed-out command disables ordinary dispatch until its ambiguous
     * mutation has been recovered.  A successful, idempotent restore proves
     * that both the retained backend record and any late command are clear;
     * re-enable the following exact-verification snapshot. */
    if(result >= 0 && g_pmu.started && !g_pmu.command_inflight &&
       !g_pmu.restore_needed && !g_pmu.selector_restore_needed)
        __atomic_store_n(&g_pmu.ready, 1, __ATOMIC_SEQ_CST);
    unlock_engine();
    return result;
}

static uint64_t microseconds_to_milliseconds(uint64_t microseconds)
{
    /* The kernel companion links with -nostdlib.  Keep the callback free of
     * the compiler's __aeabi_uldivmod helper while preserving the full wide,
     * monotonic system clock across the 32-bit low-clock wrap. */
    uint64_t quotient = 0;
    uint32_t remainder = 0;
    for(int bit = 63; bit >= 0; --bit)
    {
        remainder = (remainder << 1) |
            (uint32_t)((microseconds >> (unsigned int)bit) & UINT64_C(1));
        if(remainder >= 1000u)
        {
            remainder -= 1000u;
            quotient |= UINT64_C(1) << (unsigned int)bit;
        }
    }
    return quotient;
}

static int callback_now_ms(void* context, uint64_t* now_ms)
{
    if(context != &g_pmu || !now_ms)
        return VD_PMU_BACKEND_ERROR_INVALID;
    *now_ms = microseconds_to_milliseconds(
        (uint64_t)ksceKernelGetSystemTimeWide());
    return 0;
}

int vdPmuBackendStart(void)
{
    if(g_pmu.started)
        return VD_PMU_BACKEND_ERROR_BUSY;

    int start_error = VD_PMU_BACKEND_ERROR_CLEANUP;

    zero_bytes(&g_pmu, sizeof(g_pmu));
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        g_pmu.workers[i] = -1;
        g_pmu.events[i] = -1;
        g_pmu.worker_core[i] = (int)i;
    }
    g_pmu.started = 1;

    static const char* const event_names[VD_PMU_BACKEND_APP_CORE_COUNT] = {
        "vd pmu event0", "vd pmu event1", "vd pmu event2"
    };
    static const char* const worker_names[VD_PMU_BACKEND_APP_CORE_COUNT] = {
        "vd pmu cpu0", "vd pmu cpu1", "vd pmu cpu2"
    };
    static const int affinity[VD_PMU_BACKEND_APP_CORE_COUNT] = {
        0x00010000, 0x00020000, 0x00040000
    };

    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        g_pmu.events[i] = ksceKernelCreateEventFlag(
            event_names[i], SCE_EVENT_WAITSINGLE, 0, NULL);
        if(g_pmu.events[i] < 0)
        {
            start_error = g_pmu.events[i];
            goto start_failed;
        }
    }
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        g_pmu.workers[i] = ksceKernelCreateThread(
            worker_names[i], worker_main, 0x40, 8 * 1024, 0,
            affinity[i], NULL);
        if(g_pmu.workers[i] < 0)
        {
            start_error = g_pmu.workers[i];
            goto start_failed;
        }
        start_error = ksceKernelStartThread(
            g_pmu.workers[i], sizeof(g_pmu.worker_core[i]),
            &g_pmu.worker_core[i]);
        if(start_error < 0)
            goto start_failed;
        g_pmu.worker_started[i] = 1;
    }

    /* No PMU register is touched until an explicit callback/self-test. */
    __atomic_store_n(&g_pmu.ready, 1, __ATOMIC_SEQ_CST);
    return 0;

start_failed:
    __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pmu.worker_stop, 1, __ATOMIC_SEQ_CST);
    int cleanup_error = 0;
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
        if(g_pmu.events[i] >= 0 && g_pmu.worker_started[i] &&
           ksceKernelSetEventFlag(g_pmu.events[i],
                                  VD_PMU_BACKEND_EVENT_STOP) < 0 &&
           cleanup_error == 0)
            cleanup_error = VD_PMU_BACKEND_ERROR_CLEANUP;
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        if(g_pmu.workers[i] >= 0)
        {
            int result = 0;
            if(g_pmu.worker_started[i])
            {
                int status = 0;
                SceUInt timeout = VD_PMU_BACKEND_DISPATCH_TIMEOUT_US;
                result = ksceKernelWaitThreadEnd(g_pmu.workers[i],
                                                  &status, &timeout);
            }
            if(result >= 0)
                result = ksceKernelDeleteThread(g_pmu.workers[i]);
            if(result < 0)
                cleanup_error = VD_PMU_BACKEND_ERROR_CLEANUP;
            else
                g_pmu.workers[i] = -1;
        }
        if(g_pmu.workers[i] < 0 && g_pmu.events[i] >= 0)
        {
            if(ksceKernelDeleteEventFlag(g_pmu.events[i]) < 0)
                cleanup_error = VD_PMU_BACKEND_ERROR_CLEANUP;
            else
                g_pmu.events[i] = -1;
        }
    }
    if(cleanup_error == 0)
    {
        g_pmu.started = 0;
        return start_error;
    }
    return VD_PMU_BACKEND_START_RESIDENT_DISABLED;
}

int vdPmuBackendStop(void)
{
    if(!g_pmu.started)
        return 0;
    lock_engine();
    if(g_pmu.command_inflight)
    {
        int result = finish_inflight_locked();
        if(result == VD_PMU_BACKEND_ERROR_TIMEOUT &&
           g_pmu.command_inflight)
        {
            unlock_engine();
            return VD_PMU_BACKEND_ERROR_CLEANUP;
        }
    }
    if(g_pmu.selector_restore_needed)
    {
        int result = dispatch_locked(
            VD_PMU_BACKEND_COMMAND_RESTORE_SELECTOR,
            g_pmu.selector_restore_core, 1);
        if(result < 0)
        {
            unlock_engine();
            return VD_PMU_BACKEND_ERROR_RESTORE;
        }
    }
    if(g_pmu.restore_needed)
    {
        copy_bytes(&g_pmu.command.configuration,
                   &g_pmu.saved_configuration,
                   sizeof(g_pmu.command.configuration));
        copy_bytes(&g_pmu.command.original, &g_pmu.saved_snapshot,
                   sizeof(g_pmu.command.original));
        int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_RESTORE,
                                     g_pmu.saved_core, 1);
        if(result < 0)
        {
            unlock_engine();
            return VD_PMU_BACKEND_ERROR_RESTORE;
        }
    }
    __atomic_store_n(&g_pmu.ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pmu.worker_stop, 1, __ATOMIC_SEQ_CST);
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
        if(g_pmu.events[i] >= 0 &&
           ksceKernelSetEventFlag(g_pmu.events[i],
                                  VD_PMU_BACKEND_EVENT_STOP) < 0)
        {
            unlock_engine();
            return VD_PMU_BACKEND_ERROR_CLEANUP;
        }
    unlock_engine();

    int first_error = 0;
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        if(g_pmu.workers[i] >= 0)
        {
            int status = 0;
            SceUInt timeout = VD_PMU_BACKEND_DISPATCH_TIMEOUT_US;
            int result = ksceKernelWaitThreadEnd(g_pmu.workers[i],
                                                  &status, &timeout);
            if(result >= 0)
                result = ksceKernelDeleteThread(g_pmu.workers[i]);
            if(result < 0 && first_error == 0)
                first_error = VD_PMU_BACKEND_ERROR_CLEANUP;
            if(result >= 0)
            {
                g_pmu.workers[i] = -1;
                g_pmu.worker_started[i] = 0;
            }
        }
    }
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        if(g_pmu.workers[i] < 0 && g_pmu.events[i] >= 0)
        {
            int result = ksceKernelDeleteEventFlag(g_pmu.events[i]);
            if(result < 0 && first_error == 0)
                first_error = VD_PMU_BACKEND_ERROR_CLEANUP;
            if(result >= 0)
                g_pmu.events[i] = -1;
        }
    }
    if(first_error == 0)
        zero_bytes(&g_pmu, sizeof(g_pmu));
    return first_error;
}

int vdPmuBackendReady(void)
{
    return __atomic_load_n(&g_pmu.started, __ATOMIC_SEQ_CST) &&
           __atomic_load_n(&g_pmu.ready, __ATOMIC_SEQ_CST);
}

int vdPmuBackendHasRestoreObligation(void)
{
    lock_engine();
    const int pending = g_pmu.restore_needed != 0 ||
        g_pmu.selector_restore_needed != 0;
    unlock_engine();
    return pending;
}

int vdPmuBackendRecoveryPending(void)
{
    lock_engine();
    const int pending = g_pmu.started &&
        (__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE) ||
         g_pmu.restore_needed != 0 ||
         g_pmu.selector_restore_needed != 0);
    unlock_engine();
    return pending;
}

static void mark_ready_after_recovery_locked(void)
{
    if(g_pmu.started && !g_pmu.active && !g_pmu.restore_needed &&
       !g_pmu.selector_restore_needed &&
       !__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_pmu.ready, 1, __ATOMIC_SEQ_CST);
}

int vdPmuBackendRecover(void)
{
    int recovery_proved = 0;
    lock_engine();
    if(!g_pmu.started)
    {
        unlock_engine();
        return VD_PMU_BACKEND_ERROR_DISABLED;
    }
    /* A previously timed-out command may have completed after its caller
     * returned.  Reap that result before inspecting the obligation flags:
     * a successful late restore clears restore_needed from the worker, but
     * command_inflight must still be retired before another dispatch or a
     * safe module stop. */
    if(__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
    {
        const int inflight_result = finish_inflight_locked();
        if(inflight_result == VD_PMU_BACKEND_ERROR_TIMEOUT &&
           __atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE))
        {
            unlock_engine();
            return inflight_result;
        }
        if(inflight_result < 0 && !g_pmu.restore_needed &&
           !g_pmu.selector_restore_needed)
        {
            unlock_engine();
            return inflight_result;
        }
        if(inflight_result >= 0)
            recovery_proved = 1;
    }
    if(g_pmu.selector_restore_needed)
    {
        int selector_result = dispatch_locked(
            VD_PMU_BACKEND_COMMAND_RESTORE_SELECTOR,
            g_pmu.selector_restore_core, 1);
        if(selector_result < 0)
        {
            unlock_engine();
            return selector_result;
        }
        recovery_proved = 1;
    }
    if(!g_pmu.restore_needed)
    {
        /* Never turn readiness back on merely because a later caller finds
         * no flags.  This invocation must have reaped a successful command
         * or completed an exact selector/full restore.  Otherwise a prior
         * completed negative command remains fail-closed until unload. */
        if(recovery_proved)
            mark_ready_after_recovery_locked();
        unlock_engine();
        return 0;
    }
    copy_bytes(&g_pmu.command.configuration,
               &g_pmu.saved_configuration,
               sizeof(g_pmu.command.configuration));
    copy_bytes(&g_pmu.command.original, &g_pmu.saved_snapshot,
               sizeof(g_pmu.command.original));
    int result = dispatch_locked(VD_PMU_BACKEND_COMMAND_RESTORE,
                                 g_pmu.saved_core, 1);
    if(result >= 0)
        mark_ready_after_recovery_locked();
    unlock_engine();
    return result;
}

int vdPmuBackendMakeSessionBackend(
    struct vd_pmu_session_backend* backend)
{
    if(!backend)
        return VD_PMU_BACKEND_ERROR_INVALID;
    const struct vd_pmu_session_backend value = {
        .context = &g_pmu,
        .snapshot = callback_snapshot,
        .configure = callback_configure,
        .read = callback_read,
        .restore = callback_restore,
        .now_ms = callback_now_ms,
    };
    copy_bytes(backend, &value, sizeof(*backend));
    return vdPmuBackendReady() ? 0 : VD_PMU_BACKEND_ERROR_DISABLED;
}

int vdPmuBackendRunSelfTest(
    uint32_t core_id,
    struct vd_pmu_backend_test_result* result)
{
    if(!result || core_id >= VD_PMU_BACKEND_APP_CORE_COUNT)
        return VD_PMU_BACKEND_ERROR_INVALID;
    lock_engine();
    int status = dispatch_locked(VD_PMU_BACKEND_COMMAND_SELF_TEST,
                                 core_id, 0);
    if(status != VD_PMU_BACKEND_ERROR_TIMEOUT ||
       !g_pmu.command_inflight)
        copy_bytes(result, &g_pmu.command.test_result, sizeof(*result));
    else
        zero_bytes(result, sizeof(*result));
    unlock_engine();
    return status;
}

#if defined(VD_PMU_BACKEND_HOST_TEST)
int vdPmuBackendHostTestInit(
    const struct vd_pmu_backend_host_register_ops* ops)
{
    if(!ops || !ops->read || !ops->write)
        return VD_PMU_BACKEND_ERROR_INVALID;

    zero_bytes(&g_pmu, sizeof(g_pmu));
    copy_bytes(&g_host_register_ops, ops, sizeof(g_host_register_ops));
    g_host_dispatch_mode = VD_PMU_BACKEND_HOST_DISPATCH_INLINE;
    g_host_current_core = 0;
    g_host_time_us = 0;
    for(uint32_t i = 0; i < VD_PMU_BACKEND_APP_CORE_COUNT; ++i)
    {
        g_pmu.workers[i] = -1;
        g_pmu.events[i] = -1;
        g_pmu.worker_core[i] = (int)i;
    }
    g_pmu.started = 1;
    g_pmu.ready = 1;
    return 0;
}

void vdPmuBackendHostTestReset(void)
{
    zero_bytes(&g_pmu, sizeof(g_pmu));
    zero_bytes(&g_host_register_ops, sizeof(g_host_register_ops));
    g_host_dispatch_mode = VD_PMU_BACKEND_HOST_DISPATCH_INLINE;
    g_host_current_core = 0;
    g_host_time_us = 0;
}

void vdPmuBackendHostTestAdvanceTimeUs(uint64_t microseconds)
{
    if(UINT64_MAX - g_host_time_us < microseconds)
        g_host_time_us = UINT64_MAX;
    else
        g_host_time_us += microseconds;
}

void vdPmuBackendHostTestSetDispatchMode(
    enum vd_pmu_backend_host_dispatch_mode mode)
{
    g_host_dispatch_mode = mode;
}

int vdPmuBackendHostTestCompleteInflight(void)
{
    if(!__atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&g_pmu.command_complete, __ATOMIC_ACQUIRE))
        return VD_PMU_BACKEND_ERROR_INVALID;

    const uint32_t core =
        __atomic_load_n(&g_pmu.command_core, __ATOMIC_ACQUIRE);
    const enum vd_pmu_backend_command_kind kind =
        __atomic_load_n(&g_pmu.command_kind, __ATOMIC_ACQUIRE);
    g_host_current_core = core;
    const int result = execute_command(core, kind);
    __atomic_store_n(&g_pmu.command_result, result, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pmu.command_complete, 1, __ATOMIC_RELEASE);
    return result;
}

int vdPmuBackendHostTestIsInflight(void)
{
    return __atomic_load_n(&g_pmu.command_inflight, __ATOMIC_ACQUIRE) != 0;
}

int vdPmuBackendHostTestIsActive(void)
{
    return g_pmu.active != 0;
}

int vdPmuBackendHostTestSelectorRestorePending(void)
{
    return g_pmu.selector_restore_needed != 0;
}

void vdPmuBackendHostTestWritePmcr(uint32_t value)
{
    write_pmcr(value);
}

void vdPmuBackendHostTestWriteCounterEnableSet(uint32_t value)
{
    write_pmcntenset(value);
}

void vdPmuBackendHostTestWriteCounterEnableClear(uint32_t value)
{
    write_pmcntenclr(value);
}

void vdPmuBackendHostTestWriteInterruptEnableClear(uint32_t value)
{
    write_pmintenclr(value);
}

void vdPmuBackendHostTestWriteOverflowClear(uint32_t value)
{
    write_pmovsr(value);
}

void vdPmuBackendHostTestWriteSoftwareIncrement(uint32_t value)
{
    write_pmswinc(value);
}

void vdPmuBackendHostTestWriteSelector(uint32_t value)
{
    write_pmselr(value);
}

void vdPmuBackendHostTestWriteEventType(uint32_t value)
{
    write_pmxevtyper(value);
}

void vdPmuBackendHostTestWriteEventCount(uint32_t value)
{
    write_pmxevcntr(value);
}
#endif

#endif
