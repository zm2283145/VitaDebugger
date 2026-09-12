#include <stdint.h>

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr/event_flags.h>
#include <psp2kern/kernel/threadmgr/misc.h>
#include <psp2kern/kernel/threadmgr/thread.h>

#include "hw_debug.h"
#include "vd_armv7_debug_codec.h"

#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG

#define VD_HW_EXEC_BRP 0u
#define VD_HW_CONTEXT_BRP VD_ARMV7_CONTEXT_BRP_INDEX
#define VD_HW_WATCH_WRP 0u

#define VD_HW_ALL_CORES ((1u << VD_KERNEL_HW_CORE_COUNT) - 1u)
#define VD_HW_DISPATCH_TIMEOUT_US 250000u
#define VD_HW_DISPATCH_POLL_US 1000u
#define VD_HW_LOCK_RETRY_US 1000u
#define VD_HW_MIN_LEASE_MS 250u
#define VD_HW_MAX_LEASE_MS 5000u
#define VD_HW_EVENT_COMMAND 1u
#define VD_HW_EVENT_STOP 2u

#define VD_HW_DSCR_HDBGEN (1u << 14)
#define VD_HW_DSCR_MDBGEN (1u << 15)
#define VD_HW_DSCR_MONITOR_MASK (VD_HW_DSCR_HDBGEN | VD_HW_DSCR_MDBGEN)

#define VD_HW_BCR_COMPARE_MASK 0x007fe1e7u
#define VD_HW_WCR_COMPARE_MASK 0x1f1fe1ffu
#define VD_HW_ENABLE 1u

#define VD_HW_MIDR_MASK 0xff0ffff0u
#define VD_HW_CORTEX_A9_MIDR 0x410fc090u

enum vd_hw_command_kind {
    VD_HW_COMMAND_NONE = 0,
    VD_HW_COMMAND_INVENTORY,
    VD_HW_COMMAND_CLAIM,
    VD_HW_COMMAND_APPLY,
    VD_HW_COMMAND_RESTORE,
};

struct vd_hw_point {
    struct vd_armv7_debug_encoding encoding;
    unsigned int type;
    unsigned int address;
    unsigned int length;
    int active;
};

struct vd_hw_core_state {
    struct vd_kernel_hw_core_info inventory;
    int snapshot_valid;
    int monitor_changed;
};

struct vd_hw_session {
    SceUID pid;
    unsigned int token;
    unsigned int context_id;
    uint64_t deadline_us;
    struct vd_armv7_debug_encoding context;
    struct vd_hw_point execute;
    struct vd_hw_point watch;
    int active;
};

static struct vd_hw_core_state core_state[VD_KERNEL_HW_CORE_COUNT];
static struct vd_hw_session hw_session;
static SceUID worker_threads[VD_KERNEL_HW_CORE_COUNT] = {-1, -1, -1};
static SceUID worker_events[VD_KERNEL_HW_CORE_COUNT] = {-1, -1, -1};
static int worker_started[VD_KERNEL_HW_CORE_COUNT];
static int worker_core[VD_KERNEL_HW_CORE_COUNT];
static volatile int worker_stop;
static volatile unsigned int command_generation;
static volatile unsigned int command_complete_mask;
static volatile unsigned int command_failure_mask;
static volatile int command_result[VD_KERNEL_HW_CORE_COUNT];
static volatile enum vd_hw_command_kind command_kind;
static volatile int command_inflight;
static volatile int engine_ready;
static volatile int engine_validated;
static volatile int engine_lock;
static unsigned int next_hw_token = 0x48570001u;

static void lock_engine(void)
{
    for(;;)
    {
        int expected = 0;
        if(__atomic_compare_exchange_n(&engine_lock, &expected, 1, 0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST))
            return;
        ksceKernelDelayThread(VD_HW_LOCK_RETRY_US);
    }
}

static int try_lock_engine(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&engine_lock, &expected, 1, 0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST);
}

static void unlock_engine(void)
{
    __atomic_store_n(&engine_lock, 0, __ATOMIC_SEQ_CST);
}

static void debug_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static unsigned int read_mpidr(void)
{
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(value));
    return value;
}

static unsigned int read_midr(void)
{
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(value));
    return value;
}

static unsigned int read_didr(void)
{
    unsigned int value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 0" : "=r"(value));
    return value;
}

static unsigned int read_dscr_int(void)
{
    unsigned int value;
    __asm__ volatile("mrc p14, 0, %0, c0, c1, 0" : "=r"(value));
    return value;
}

static unsigned int read_vcr(void)
{
    unsigned int value;
    __asm__ volatile("mrc p14, 0, %0, c0, c7, 0" : "=r"(value));
    return value;
}

static void write_dscr_ext(unsigned int value)
{
    __asm__ volatile("mcr p14, 0, %0, c0, c2, 2" : : "r"(value));
}

static unsigned int read_bvr(unsigned int index)
{
    unsigned int value = 0;
    switch(index)
    {
        case 0: __asm__ volatile("mrc p14, 0, %0, c0, c0, 4" : "=r"(value)); break;
        case 1: __asm__ volatile("mrc p14, 0, %0, c0, c1, 4" : "=r"(value)); break;
        case 2: __asm__ volatile("mrc p14, 0, %0, c0, c2, 4" : "=r"(value)); break;
        case 3: __asm__ volatile("mrc p14, 0, %0, c0, c3, 4" : "=r"(value)); break;
        case 4: __asm__ volatile("mrc p14, 0, %0, c0, c4, 4" : "=r"(value)); break;
        case 5: __asm__ volatile("mrc p14, 0, %0, c0, c5, 4" : "=r"(value)); break;
        default: break;
    }
    return value;
}

static unsigned int read_bcr(unsigned int index)
{
    unsigned int value = 0;
    switch(index)
    {
        case 0: __asm__ volatile("mrc p14, 0, %0, c0, c0, 5" : "=r"(value)); break;
        case 1: __asm__ volatile("mrc p14, 0, %0, c0, c1, 5" : "=r"(value)); break;
        case 2: __asm__ volatile("mrc p14, 0, %0, c0, c2, 5" : "=r"(value)); break;
        case 3: __asm__ volatile("mrc p14, 0, %0, c0, c3, 5" : "=r"(value)); break;
        case 4: __asm__ volatile("mrc p14, 0, %0, c0, c4, 5" : "=r"(value)); break;
        case 5: __asm__ volatile("mrc p14, 0, %0, c0, c5, 5" : "=r"(value)); break;
        default: break;
    }
    return value;
}

static unsigned int read_wvr(unsigned int index)
{
    unsigned int value = 0;
    switch(index)
    {
        case 0: __asm__ volatile("mrc p14, 0, %0, c0, c0, 6" : "=r"(value)); break;
        case 1: __asm__ volatile("mrc p14, 0, %0, c0, c1, 6" : "=r"(value)); break;
        case 2: __asm__ volatile("mrc p14, 0, %0, c0, c2, 6" : "=r"(value)); break;
        case 3: __asm__ volatile("mrc p14, 0, %0, c0, c3, 6" : "=r"(value)); break;
        default: break;
    }
    return value;
}

static unsigned int read_wcr(unsigned int index)
{
    unsigned int value = 0;
    switch(index)
    {
        case 0: __asm__ volatile("mrc p14, 0, %0, c0, c0, 7" : "=r"(value)); break;
        case 1: __asm__ volatile("mrc p14, 0, %0, c0, c1, 7" : "=r"(value)); break;
        case 2: __asm__ volatile("mrc p14, 0, %0, c0, c2, 7" : "=r"(value)); break;
        case 3: __asm__ volatile("mrc p14, 0, %0, c0, c3, 7" : "=r"(value)); break;
        default: break;
    }
    return value;
}

static void write_bvr(unsigned int index, unsigned int value)
{
    switch(index)
    {
        case 0: __asm__ volatile("mcr p14, 0, %0, c0, c0, 4" : : "r"(value)); break;
        case 5: __asm__ volatile("mcr p14, 0, %0, c0, c5, 4" : : "r"(value)); break;
        default: break;
    }
}

static void write_bcr(unsigned int index, unsigned int value)
{
    switch(index)
    {
        case 0: __asm__ volatile("mcr p14, 0, %0, c0, c0, 5" : : "r"(value)); break;
        case 5: __asm__ volatile("mcr p14, 0, %0, c0, c5, 5" : : "r"(value)); break;
        default: break;
    }
}

static void write_wvr(unsigned int index, unsigned int value)
{
    if(index == 0)
        __asm__ volatile("mcr p14, 0, %0, c0, c0, 6" : : "r"(value));
}

static void write_wcr(unsigned int index, unsigned int value)
{
    if(index == 0)
        __asm__ volatile("mcr p14, 0, %0, c0, c0, 7" : : "r"(value));
}

static void write_dscr_sync(unsigned int value)
{
    write_dscr_ext(value);
    debug_isb();
}

static void write_bvr_sync(unsigned int index, unsigned int value)
{
    write_bvr(index, value);
    debug_isb();
}

static void write_bcr_sync(unsigned int index, unsigned int value)
{
    write_bcr(index, value);
    debug_isb();
}

static void write_wvr_sync(unsigned int index, unsigned int value)
{
    write_wvr(index, value);
    debug_isb();
}

static void write_wcr_sync(unsigned int index, unsigned int value)
{
    write_wcr(index, value);
    debug_isb();
}

static unsigned int breakpoint_count(unsigned int didr)
{
    return ((didr >> 24) & 0xfu) + 1u;
}

static unsigned int watchpoint_count(unsigned int didr)
{
    return ((didr >> 28) & 0xfu) + 1u;
}

static unsigned int context_breakpoint_count(unsigned int didr)
{
    return ((didr >> 20) & 0xfu) + 1u;
}

static int capture_inventory(unsigned int expected_core,
                             struct vd_kernel_hw_core_info* info)
{
    info->core_id = (unsigned int)ksceKernelCpuId();
    info->raw_mpidr = read_mpidr();
    info->raw_midr = read_midr();
    if(info->core_id != expected_core ||
       (info->raw_mpidr & 0xffu) != expected_core ||
       (info->raw_midr & VD_HW_MIDR_MASK) != VD_HW_CORTEX_A9_MIDR)
        return VD_KERNEL_ERROR_HW_CORE;

    info->raw_didr = read_didr();
    if(breakpoint_count(info->raw_didr) < VD_KERNEL_HW_BREAKPOINT_COUNT ||
       watchpoint_count(info->raw_didr) < VD_KERNEL_HW_WATCHPOINT_COUNT ||
       context_breakpoint_count(info->raw_didr) < 2u)
        return VD_KERNEL_ERROR_HW_CORE;

    info->raw_dscr = read_dscr_int();
    info->raw_vcr = read_vcr();
    for(unsigned int i = 0; i < VD_KERNEL_HW_BREAKPOINT_COUNT; ++i)
        info->breakpoint_control[i] = read_bcr(i);
    for(unsigned int i = 0; i < VD_KERNEL_HW_WATCHPOINT_COUNT; ++i)
        info->watchpoint_control[i] = read_wcr(i);
    info->breakpoint0_value = read_bvr(VD_HW_EXEC_BRP);
    info->context_breakpoint_value = read_bvr(VD_HW_CONTEXT_BRP);
    info->watchpoint0_value = read_wvr(VD_HW_WATCH_WRP);
    return 0;
}

static int validate_inventory(unsigned int expected_core,
                              const struct vd_kernel_hw_core_info* info,
                              int require_free)
{
    if(info->core_id != expected_core ||
       (info->raw_mpidr & 0xffu) != expected_core)
        return VD_KERNEL_ERROR_HW_CORE;
    if((info->raw_midr & VD_HW_MIDR_MASK) != VD_HW_CORTEX_A9_MIDR)
        return VD_KERNEL_ERROR_HW_CORE;
    if(breakpoint_count(info->raw_didr) < VD_KERNEL_HW_BREAKPOINT_COUNT ||
       watchpoint_count(info->raw_didr) < VD_KERNEL_HW_WATCHPOINT_COUNT ||
       context_breakpoint_count(info->raw_didr) < 2u)
        return VD_KERNEL_ERROR_HW_CORE;
    if((info->raw_dscr & VD_HW_DSCR_HDBGEN) != 0 || info->raw_vcr != 0)
        return VD_KERNEL_ERROR_HW_BUSY;
    if(require_free)
    {
        /*
         * The first experimental implementation takes no chances with an
         * existing debugger.  A comparator outside our three reserved slots
         * may still link to BRP5, so require the whole core to be idle.
         */
        for(unsigned int i = 0; i < VD_KERNEL_HW_BREAKPOINT_COUNT; ++i)
            if((info->breakpoint_control[i] & VD_HW_ENABLE) != 0)
                return VD_KERNEL_ERROR_HW_BUSY;
        for(unsigned int i = 0; i < VD_KERNEL_HW_WATCHPOINT_COUNT; ++i)
            if((info->watchpoint_control[i] & VD_HW_ENABLE) != 0)
                return VD_KERNEL_ERROR_HW_BUSY;
    }
    return 0;
}

static int readback_control(unsigned int actual, unsigned int expected,
                            unsigned int mask)
{
    return ((actual ^ expected) & mask) == 0 ? 0 :
        VD_KERNEL_ERROR_HW_CORE;
}

static int restore_comparators(unsigned int core)
{
    const struct vd_kernel_hw_core_info* saved;
    int result = 0;
    if(!core_state[core].snapshot_valid)
        return 0;
    saved = &core_state[core].inventory;

    /* Disable the linked consumers before their context provider. */
    write_bcr_sync(VD_HW_EXEC_BRP,
                   read_bcr(VD_HW_EXEC_BRP) & ~VD_HW_ENABLE);
    write_wcr_sync(VD_HW_WATCH_WRP,
                   read_wcr(VD_HW_WATCH_WRP) & ~VD_HW_ENABLE);
    write_bcr_sync(VD_HW_CONTEXT_BRP,
                   read_bcr(VD_HW_CONTEXT_BRP) & ~VD_HW_ENABLE);
    if((read_bcr(VD_HW_EXEC_BRP) & VD_HW_ENABLE) != 0 ||
       (read_bcr(VD_HW_CONTEXT_BRP) & VD_HW_ENABLE) != 0 ||
       (read_wcr(VD_HW_WATCH_WRP) & VD_HW_ENABLE) != 0)
        result = VD_KERNEL_ERROR_HW_RESTORE;

    write_bvr_sync(VD_HW_EXEC_BRP, saved->breakpoint0_value);
    write_wvr_sync(VD_HW_WATCH_WRP, saved->watchpoint0_value);
    write_bvr_sync(VD_HW_CONTEXT_BRP,
                   saved->context_breakpoint_value);

    /* Acquisition refuses enabled slots, so every saved control has E=0. */
    write_bcr_sync(VD_HW_CONTEXT_BRP,
                   saved->breakpoint_control[VD_HW_CONTEXT_BRP]);
    write_bcr_sync(VD_HW_EXEC_BRP,
                   saved->breakpoint_control[VD_HW_EXEC_BRP]);
    write_wcr_sync(VD_HW_WATCH_WRP,
                   saved->watchpoint_control[VD_HW_WATCH_WRP]);

    if(read_bvr(VD_HW_EXEC_BRP) != saved->breakpoint0_value ||
       read_bvr(VD_HW_CONTEXT_BRP) !=
           saved->context_breakpoint_value ||
       read_wvr(VD_HW_WATCH_WRP) != saved->watchpoint0_value ||
       readback_control(read_bcr(VD_HW_EXEC_BRP),
                        saved->breakpoint_control[VD_HW_EXEC_BRP],
                        VD_HW_BCR_COMPARE_MASK) < 0 ||
       readback_control(read_bcr(VD_HW_CONTEXT_BRP),
                        saved->breakpoint_control[VD_HW_CONTEXT_BRP],
                        VD_HW_BCR_COMPARE_MASK) < 0 ||
       readback_control(read_wcr(VD_HW_WATCH_WRP),
                        saved->watchpoint_control[VD_HW_WATCH_WRP],
                        VD_HW_WCR_COMPARE_MASK) < 0)
        result = VD_KERNEL_ERROR_HW_RESTORE;
    return result;
}

static int disabled_roundtrip(unsigned int core)
{
    struct vd_armv7_debug_encoding execute;
    struct vd_armv7_debug_encoding context;
    struct vd_armv7_debug_encoding watch;
    int test_result = 0;

    if(vd_armv7_encode_linked_exec(0x55aa10c0u, 4u, &execute) < 0 ||
       vd_armv7_encode_linked_context(0xa55a3cc3u, &context) < 0 ||
       vd_armv7_encode_linked_watch(0xc33c20e0u, 4u,
                                    VD_ARMV7_WATCH_ACCESS, &watch) < 0)
        return VD_KERNEL_ERROR_HW_INVALID;

    /*
     * Exercise implemented value and control bits with E kept clear.  Merely
     * rewriting the usually-zero snapshot would not prove the CP14 write path.
     * Nothing in this probe can match because no comparator is enabled.
     */
    write_bcr_sync(VD_HW_EXEC_BRP, 0);
    write_wcr_sync(VD_HW_WATCH_WRP, 0);
    write_bcr_sync(VD_HW_CONTEXT_BRP, 0);
    if((read_bcr(VD_HW_EXEC_BRP) & VD_HW_ENABLE) != 0 ||
       (read_bcr(VD_HW_CONTEXT_BRP) & VD_HW_ENABLE) != 0 ||
       (read_wcr(VD_HW_WATCH_WRP) & VD_HW_ENABLE) != 0)
        test_result = VD_KERNEL_ERROR_HW_CORE;

    if(test_result >= 0)
    {
        write_bvr_sync(VD_HW_EXEC_BRP, execute.value);
        write_bvr_sync(VD_HW_CONTEXT_BRP, context.value);
        write_wvr_sync(VD_HW_WATCH_WRP, watch.value);
        if(read_bvr(VD_HW_EXEC_BRP) != execute.value ||
           read_bvr(VD_HW_CONTEXT_BRP) != context.value ||
           read_wvr(VD_HW_WATCH_WRP) != watch.value)
            test_result = VD_KERNEL_ERROR_HW_CORE;
    }

    if(test_result >= 0)
    {
        write_bcr_sync(VD_HW_CONTEXT_BRP,
                       context.control & ~VD_HW_ENABLE);
        write_bcr_sync(VD_HW_EXEC_BRP,
                       execute.control & ~VD_HW_ENABLE);
        write_wcr_sync(VD_HW_WATCH_WRP,
                       watch.control & ~VD_HW_ENABLE);
        if(readback_control(read_bcr(VD_HW_CONTEXT_BRP),
                            context.control & ~VD_HW_ENABLE,
                            VD_HW_BCR_COMPARE_MASK) < 0 ||
           readback_control(read_bcr(VD_HW_EXEC_BRP),
                            execute.control & ~VD_HW_ENABLE,
                            VD_HW_BCR_COMPARE_MASK) < 0 ||
           readback_control(read_wcr(VD_HW_WATCH_WRP),
                            watch.control & ~VD_HW_ENABLE,
                            VD_HW_WCR_COMPARE_MASK) < 0)
            test_result = VD_KERNEL_ERROR_HW_CORE;
    }

    if(restore_comparators(core) < 0)
        return VD_KERNEL_ERROR_HW_RESTORE;
    return test_result;
}

static int enable_monitor_mode(unsigned int core)
{
    unsigned int current = read_dscr_int();
    unsigned int desired;
    if((current & VD_HW_DSCR_HDBGEN) != 0 || read_vcr() != 0)
        return VD_KERNEL_ERROR_HW_BUSY;
    desired = (current | VD_HW_DSCR_MDBGEN) & ~VD_HW_DSCR_HDBGEN;
    if((current & VD_HW_DSCR_MONITOR_MASK) ==
       (desired & VD_HW_DSCR_MONITOR_MASK))
        return 0;

    /* Mark this before the write so even a failed readback is rolled back. */
    core_state[core].monitor_changed = 1;
    write_dscr_sync(desired);
    current = read_dscr_int();
    return ((current ^ desired) & VD_HW_DSCR_MONITOR_MASK) == 0 ? 0 :
        VD_KERNEL_ERROR_HW_CORE;
}

static int restore_core(unsigned int core)
{
    const struct vd_kernel_hw_core_info* saved;
    int result;
    if(!core_state[core].snapshot_valid)
        return 0;
    saved = &core_state[core].inventory;

    result = restore_comparators(core);
    if(core_state[core].monitor_changed)
    {
        unsigned int current = read_dscr_int();
        unsigned int desired;
        if((current & VD_HW_DSCR_HDBGEN) != 0)
            result = VD_KERNEL_ERROR_HW_RESTORE;
        else
        {
            desired = (current & ~VD_HW_DSCR_MONITOR_MASK) |
                (saved->raw_dscr & VD_HW_DSCR_MONITOR_MASK);
            write_dscr_sync(desired);
            current = read_dscr_int();
            if(((current ^ desired) & VD_HW_DSCR_MONITOR_MASK) != 0)
                result = VD_KERNEL_ERROR_HW_RESTORE;
            else
                core_state[core].monitor_changed = 0;
        }
    }
    return result < 0 ? VD_KERNEL_ERROR_HW_RESTORE : 0;
}

static int apply_core(unsigned int core)
{
    const struct vd_kernel_hw_core_info* saved =
        &core_state[core].inventory;
    unsigned int execute_control;
    unsigned int watch_control;
    int any_active = hw_session.execute.active || hw_session.watch.active;
    int result = VD_KERNEL_ERROR_HW_CORE;

    if(!core_state[core].snapshot_valid)
        return VD_KERNEL_ERROR_HW_CORE;
    if(!any_active)
        return restore_core(core);

    /* Disable linked consumers first, then their Context ID provider. */
    write_bcr_sync(VD_HW_EXEC_BRP,
                   read_bcr(VD_HW_EXEC_BRP) & ~VD_HW_ENABLE);
    write_wcr_sync(VD_HW_WATCH_WRP,
                   read_wcr(VD_HW_WATCH_WRP) & ~VD_HW_ENABLE);
    write_bcr_sync(VD_HW_CONTEXT_BRP,
                   read_bcr(VD_HW_CONTEXT_BRP) & ~VD_HW_ENABLE);
    if((read_bcr(VD_HW_EXEC_BRP) & VD_HW_ENABLE) != 0 ||
       (read_bcr(VD_HW_CONTEXT_BRP) & VD_HW_ENABLE) != 0 ||
       (read_wcr(VD_HW_WATCH_WRP) & VD_HW_ENABLE) != 0)
        goto rollback;

    write_bvr_sync(VD_HW_CONTEXT_BRP, hw_session.context.value);
    write_bvr_sync(VD_HW_EXEC_BRP, hw_session.execute.active ?
                   hw_session.execute.encoding.value :
                   saved->breakpoint0_value);
    write_wvr_sync(VD_HW_WATCH_WRP, hw_session.watch.active ?
                   hw_session.watch.encoding.value :
                   saved->watchpoint0_value);
    if(read_bvr(VD_HW_CONTEXT_BRP) != hw_session.context.value ||
       read_bvr(VD_HW_EXEC_BRP) != (hw_session.execute.active ?
           hw_session.execute.encoding.value : saved->breakpoint0_value) ||
       read_wvr(VD_HW_WATCH_WRP) != (hw_session.watch.active ?
           hw_session.watch.encoding.value : saved->watchpoint0_value))
        goto rollback;

    execute_control = hw_session.execute.active ?
        hw_session.execute.encoding.control & ~VD_HW_ENABLE :
        saved->breakpoint_control[VD_HW_EXEC_BRP];
    watch_control = hw_session.watch.active ?
        hw_session.watch.encoding.control & ~VD_HW_ENABLE :
        saved->watchpoint_control[VD_HW_WATCH_WRP];
    write_bcr_sync(VD_HW_CONTEXT_BRP,
                   hw_session.context.control & ~VD_HW_ENABLE);
    write_bcr_sync(VD_HW_EXEC_BRP, execute_control);
    write_wcr_sync(VD_HW_WATCH_WRP, watch_control);
    if(readback_control(read_bcr(VD_HW_CONTEXT_BRP),
                        hw_session.context.control & ~VD_HW_ENABLE,
                        VD_HW_BCR_COMPARE_MASK) < 0 ||
       readback_control(read_bcr(VD_HW_EXEC_BRP), execute_control,
                        VD_HW_BCR_COMPARE_MASK) < 0 ||
       readback_control(read_wcr(VD_HW_WATCH_WRP), watch_control,
                        VD_HW_WCR_COMPARE_MASK) < 0)
        goto rollback;

    result = enable_monitor_mode(core);
    if(result < 0)
        goto rollback;

    /* The provider must be live before either linked consumer is enabled. */
    write_bcr_sync(VD_HW_CONTEXT_BRP, hw_session.context.control);
    if(readback_control(read_bcr(VD_HW_CONTEXT_BRP),
                        hw_session.context.control,
                        VD_HW_BCR_COMPARE_MASK) < 0)
    {
        result = VD_KERNEL_ERROR_HW_CORE;
        goto rollback;
    }

    if(hw_session.execute.active)
    {
        write_bcr_sync(VD_HW_EXEC_BRP,
                       hw_session.execute.encoding.control);
        if(readback_control(read_bcr(VD_HW_EXEC_BRP),
                            hw_session.execute.encoding.control,
                            VD_HW_BCR_COMPARE_MASK) < 0)
        {
            result = VD_KERNEL_ERROR_HW_CORE;
            goto rollback;
        }
    }
    if(hw_session.watch.active)
    {
        write_wcr_sync(VD_HW_WATCH_WRP,
                       hw_session.watch.encoding.control);
        if(readback_control(read_wcr(VD_HW_WATCH_WRP),
                            hw_session.watch.encoding.control,
                            VD_HW_WCR_COMPARE_MASK) < 0)
        {
            result = VD_KERNEL_ERROR_HW_CORE;
            goto rollback;
        }
    }
    return 0;

rollback:
    if(restore_core(core) < 0)
        return VD_KERNEL_ERROR_HW_RESTORE;
    return result;
}

static int execute_command(unsigned int core, enum vd_hw_command_kind kind)
{
    SceKernelIntrStatus intr_state;
    int result = 0;
    if(core >= VD_KERNEL_HW_CORE_COUNT)
        return VD_KERNEL_ERROR_HW_CORE;

    intr_state = ksceKernelCpuSuspendIntr();
    if((unsigned int)ksceKernelCpuId() != core)
    {
        result = VD_KERNEL_ERROR_HW_CORE;
    }
    else if(kind == VD_HW_COMMAND_INVENTORY)
    {
        result = capture_inventory(core, &core_state[core].inventory);
        if(result >= 0)
            result = validate_inventory(core, &core_state[core].inventory, 0);
    }
    else if(kind == VD_HW_COMMAND_CLAIM)
    {
        core_state[core].snapshot_valid = 0;
        core_state[core].monitor_changed = 0;
        result = capture_inventory(core, &core_state[core].inventory);
        if(result >= 0)
            result = validate_inventory(core, &core_state[core].inventory, 1);
        if(result >= 0)
        {
            core_state[core].snapshot_valid = 1;
            result = disabled_roundtrip(core);
        }
    }
    else if(kind == VD_HW_COMMAND_APPLY)
    {
        result = apply_core(core);
    }
    else if(kind == VD_HW_COMMAND_RESTORE)
    {
        result = restore_core(core);
    }
    else
    {
        result = VD_KERNEL_ERROR_HW_INVALID;
    }
    ksceKernelCpuResumeIntr(intr_state);
    return result;
}

static int worker_main(SceSize args, void* argp)
{
    unsigned int core;
    unsigned int seen = 0;
    unsigned int events;
    if(args != sizeof(int) || !argp)
        return VD_KERNEL_ERROR_HW_CORE;
    core = (unsigned int)*(int*)argp;
    if(core >= VD_KERNEL_HW_CORE_COUNT)
        return VD_KERNEL_ERROR_HW_CORE;

    for(;;)
    {
        int wait_result = ksceKernelWaitEventFlag(
            worker_events[core], VD_HW_EVENT_COMMAND | VD_HW_EVENT_STOP,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &events, NULL);
        if(wait_result < 0)
            return wait_result;
        if((events & VD_HW_EVENT_STOP) != 0 ||
           __atomic_load_n(&worker_stop, __ATOMIC_SEQ_CST))
            break;

        unsigned int generation = __atomic_load_n(&command_generation,
                                                   __ATOMIC_ACQUIRE);
        if(generation != seen)
        {
            enum vd_hw_command_kind kind =
                __atomic_load_n(&command_kind, __ATOMIC_ACQUIRE);
            int result = execute_command(core, kind);
            command_result[core] = result;
            if(result < 0)
                __atomic_fetch_or(&command_failure_mask, 1u << core,
                                  __ATOMIC_SEQ_CST);
            __atomic_fetch_or(&command_complete_mask, 1u << core,
                              __ATOMIC_RELEASE);
            seen = generation;
        }
    }
    return 0;
}

static int finish_inflight_command(int* failed_core)
{
    uint64_t deadline;
    unsigned int complete;
    unsigned int failures;

    if(!__atomic_load_n(&command_inflight, __ATOMIC_ACQUIRE))
    {
        if(failed_core)
            *failed_core = -1;
        return 0;
    }

    deadline = (uint64_t)ksceKernelGetSystemTimeWide() +
               VD_HW_DISPATCH_TIMEOUT_US;
    for(;;)
    {
        complete = __atomic_load_n(&command_complete_mask,
                                   __ATOMIC_ACQUIRE);
        if(complete == VD_HW_ALL_CORES)
            break;
        if((uint64_t)ksceKernelGetSystemTimeWide() >= deadline)
        {
            if(failed_core)
            {
                *failed_core = -1;
                for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
                    if((complete & (1u << i)) == 0)
                    {
                        *failed_core = (int)i;
                        break;
                    }
            }
            /*
             * Do not issue another generation over a late worker.  A later
             * watchdog/release pass may observe completion and then restore.
             */
            __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
            return VD_KERNEL_ERROR_HW_CORE;
        }
        ksceKernelDelayThread(VD_HW_DISPATCH_POLL_US);
    }

    failures = __atomic_load_n(&command_failure_mask, __ATOMIC_ACQUIRE);
    __atomic_store_n(&command_inflight, 0, __ATOMIC_RELEASE);
    if(failures)
    {
        for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
            if(failures & (1u << i))
            {
                if(failed_core)
                    *failed_core = (int)i;
                return command_result[i];
            }
    }
    if(failed_core)
        *failed_core = -1;
    return 0;
}

static int dispatch_command(enum vd_hw_command_kind kind, int* failed_core)
{
    unsigned int generation;

    if(__atomic_load_n(&command_inflight, __ATOMIC_ACQUIRE))
    {
        /*
         * This can only be a command whose original caller timed out.  Wait
         * for it to quiesce before publishing the cleanup generation.  Its
         * status is intentionally superseded by the requested cleanup.
         */
        if(finish_inflight_command(failed_core) == VD_KERNEL_ERROR_HW_CORE &&
           __atomic_load_n(&command_inflight, __ATOMIC_ACQUIRE))
            return VD_KERNEL_ERROR_HW_CORE;
    }

    generation = __atomic_load_n(&command_generation,
                                 __ATOMIC_RELAXED) + 1u;
    if(generation == 0)
        generation = 1;
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
        command_result[i] = 0;
    __atomic_store_n(&command_complete_mask, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_failure_mask, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_kind, kind, __ATOMIC_RELEASE);
    __atomic_store_n(&command_inflight, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&command_generation, generation, __ATOMIC_RELEASE);
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        if(ksceKernelSetEventFlag(worker_events[i],
                                  VD_HW_EVENT_COMMAND) < 0)
        {
            command_result[i] = VD_KERNEL_ERROR_HW_CORE;
            __atomic_fetch_or(&command_failure_mask, 1u << i,
                              __ATOMIC_SEQ_CST);
            __atomic_fetch_or(&command_complete_mask, 1u << i,
                              __ATOMIC_RELEASE);
        }
    }
    return finish_inflight_command(failed_core);
}

static void clear_point(struct vd_hw_point* point)
{
    point->encoding.value = 0;
    point->encoding.control = 0;
    point->type = 0;
    point->address = 0;
    point->length = 0;
    point->active = 0;
}

static void clear_session(void)
{
    hw_session.pid = -1;
    hw_session.token = 0;
    hw_session.context_id = 0;
    hw_session.deadline_us = 0;
    hw_session.context.value = 0;
    hw_session.context.control = 0;
    clear_point(&hw_session.execute);
    clear_point(&hw_session.watch);
    hw_session.active = 0;
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        core_state[i].snapshot_valid = 0;
        core_state[i].monitor_changed = 0;
    }
}

static int restore_session_locked(void)
{
    int failed_core = -1;
    int result = dispatch_command(VD_HW_COMMAND_RESTORE, &failed_core);
    if(result >= 0)
        clear_session();
    else
        __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
    return result < 0 ? VD_KERNEL_ERROR_HW_RESTORE : 0;
}

static void copy_core_info(struct vd_kernel_hw_core_info* destination,
                           const struct vd_kernel_hw_core_info* source)
{
    destination->core_id = source->core_id;
    destination->raw_mpidr = source->raw_mpidr;
    destination->raw_midr = source->raw_midr;
    destination->raw_didr = source->raw_didr;
    destination->raw_dscr = source->raw_dscr;
    destination->raw_vcr = source->raw_vcr;
    for(unsigned int i = 0; i < VD_KERNEL_HW_BREAKPOINT_COUNT; ++i)
        destination->breakpoint_control[i] = source->breakpoint_control[i];
    for(unsigned int i = 0; i < VD_KERNEL_HW_WATCHPOINT_COUNT; ++i)
        destination->watchpoint_control[i] = source->watchpoint_control[i];
    destination->breakpoint0_value = source->breakpoint0_value;
    destination->context_breakpoint_value =
        source->context_breakpoint_value;
    destination->watchpoint0_value = source->watchpoint0_value;
}

int vdHwDebugStart(void)
{
    __atomic_store_n(&worker_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&engine_validated, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_generation, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_complete_mask, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_failure_mask, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_kind, VD_HW_COMMAND_NONE,
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&command_inflight, 0, __ATOMIC_SEQ_CST);
    clear_session();
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        worker_threads[i] = -1;
        worker_events[i] = -1;
        worker_started[i] = 0;
    }

    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        worker_events[i] = ksceKernelCreateEventFlag(
            i == 0 ? "vd hw event0" :
            (i == 1 ? "vd hw event1" : "vd hw event2"),
            SCE_EVENT_WAITSINGLE, 0, NULL);
        if(worker_events[i] < 0)
            goto start_failed;
    }

    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        static const int affinity[VD_KERNEL_HW_CORE_COUNT] = {
            0x00010000, 0x00020000, 0x00040000
        };
        worker_core[i] = (int)i;
        worker_threads[i] = ksceKernelCreateThread(
            i == 0 ? "vd hw cpu0" : (i == 1 ? "vd hw cpu1" : "vd hw cpu2"),
            worker_main, 0x40, 8 * 1024, 0, affinity[i], NULL);
        if(worker_threads[i] < 0)
            goto start_failed;
        if(ksceKernelStartThread(worker_threads[i], sizeof(worker_core[i]),
                                 &worker_core[i]) < 0)
            goto start_failed;
        worker_started[i] = 1;
    }

    /*
     * Deliberately perform no CP14 access while the *KERNEL plugin is loading.
     * The first hardware touch is an explicit acquisition syscall, allowing a
     * disposable probe to fail without creating a boot-loop at module_start.
     */
    __atomic_store_n(&engine_ready, 1, __ATOMIC_SEQ_CST);
    return 0;

start_failed:
    {
        int cleanup_error = 0;
        __atomic_store_n(&worker_stop, 1, __ATOMIC_SEQ_CST);
        for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
            if(worker_events[i] >= 0 &&
               ksceKernelSetEventFlag(worker_events[i],
                                      VD_HW_EVENT_STOP) < 0)
            {
                int result = ksceKernelDeleteEventFlag(worker_events[i]);
                if(result >= 0)
                    worker_events[i] = -1;
                else if(cleanup_error == 0)
                    cleanup_error = result;
            }
        for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
        {
            if(worker_threads[i] >= 0)
            {
                int result = 0;
                if(worker_started[i])
                {
                    int status = 0;
                    SceUInt timeout_us = VD_HW_DISPATCH_TIMEOUT_US;
                    result = ksceKernelWaitThreadEnd(worker_threads[i],
                                                      &status, &timeout_us);
                }
                if(result >= 0)
                    result = ksceKernelDeleteThread(worker_threads[i]);
                if(result >= 0)
                {
                    worker_threads[i] = -1;
                    worker_started[i] = 0;
                }
                else if(cleanup_error == 0)
                    cleanup_error = result;
            }
            if(worker_threads[i] < 0 && worker_events[i] >= 0)
            {
                int result = ksceKernelDeleteEventFlag(worker_events[i]);
                if(result >= 0)
                    worker_events[i] = -1;
                else if(cleanup_error == 0)
                    cleanup_error = result;
            }
        }
        /*
         * A positive result tells module_start to remain resident but fail
         * closed if any worker/object could not be proven gone.  Unloading
         * live worker code would be more dangerous than a disabled instance.
         */
        if(cleanup_error < 0)
            return 1;
        return VD_KERNEL_ERROR_HW_CORE;
    }
}

int vdHwDebugStop(void)
{
    int first_error = 0;
    lock_engine();
    if(hw_session.active && restore_session_locked() < 0)
    {
        unlock_engine();
        return VD_KERNEL_ERROR_HW_RESTORE;
    }
    if(__atomic_load_n(&command_inflight, __ATOMIC_ACQUIRE) &&
       finish_inflight_command(NULL) < 0)
    {
        unlock_engine();
        return VD_KERNEL_ERROR_HW_RESTORE;
    }
    __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&engine_validated, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&worker_stop, 1, __ATOMIC_SEQ_CST);
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
        if(worker_events[i] >= 0 &&
           ksceKernelSetEventFlag(worker_events[i], VD_HW_EVENT_STOP) < 0)
        {
            int delete_result = ksceKernelDeleteEventFlag(worker_events[i]);
            if(delete_result < 0)
            {
                if(first_error == 0)
                    first_error = delete_result;
            }
            else
                worker_events[i] = -1;
        }
    unlock_engine();

    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        if(worker_threads[i] >= 0)
        {
            int status = 0;
            SceUInt timeout_us = VD_HW_DISPATCH_TIMEOUT_US;
            int result = ksceKernelWaitThreadEnd(worker_threads[i], &status,
                                                  &timeout_us);
            if(result >= 0)
                result = ksceKernelDeleteThread(worker_threads[i]);
            if(result < 0 && first_error == 0)
                first_error = result;
            if(result >= 0)
            {
                worker_threads[i] = -1;
                worker_started[i] = 0;
            }
        }
    }
    if(first_error != 0)
        return first_error;
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        if(worker_events[i] >= 0)
        {
            int result = ksceKernelDeleteEventFlag(worker_events[i]);
            if(result < 0 && first_error == 0)
                first_error = result;
            if(result >= 0)
                worker_events[i] = -1;
        }
    }
    return first_error;
}

int vdHwDebugReady(void)
{
    return __atomic_load_n(&engine_ready, __ATOMIC_SEQ_CST) != 0 &&
           __atomic_load_n(&engine_validated, __ATOMIC_SEQ_CST) != 0;
}

void vdHwDebugWatchdog(uint64_t now_us)
{
    if(!try_lock_engine())
        return;
    if(hw_session.active && now_us >= hw_session.deadline_us)
        restore_session_locked();
    unlock_engine();
}

int vdHwDebugRecover(SceUID caller_pid, unsigned int token)
{
    int result = VD_KERNEL_ERROR_HW_BUSY;

    /*
     * This is called only while the ordinary stop-session gate is holding the
     * target after an uncertain APPLY.  A zero return means that no saved
     * session remains and every local-core snapshot was restored exactly.
     * Never disturb a later or unrelated owner while resolving an old gate.
     */
    if(caller_pid < 0 || token == 0)
        return VD_KERNEL_ERROR_HW_INVALID;
    if(!try_lock_engine())
        return result;
    if(hw_session.active && hw_session.pid == caller_pid &&
       hw_session.token == token)
        result = restore_session_locked();
    else if(hw_session.active)
        result = VD_KERNEL_ERROR_HW_BUSY;
    else if(__atomic_load_n(&command_inflight, __ATOMIC_ACQUIRE))
        result = VD_KERNEL_ERROR_HW_BUSY;
    else
        result = 0;
    unlock_engine();
    return result;
}

int vdHwDebugAcquire(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int lease_ms,
    struct vd_kernel_hw_session_result* result)
{
    struct vd_armv7_debug_encoding context;
    int status;
    int failed_core = -1;
    if(!result)
        return VD_KERNEL_ERROR_HW_INVALID;

    result->token = 0;
    result->core_count = VD_KERNEL_HW_CORE_COUNT;
    result->context_id = context_id;
    result->failure_core = -1;
    result->failure_code = 0;
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        result->core[i] = (struct vd_kernel_hw_core_info){0};
    }
    if(caller_pid < 0 || context_id == 0 ||
       lease_ms < VD_HW_MIN_LEASE_MS || lease_ms > VD_HW_MAX_LEASE_MS)
    {
        result->failure_code = VD_KERNEL_ERROR_HW_INVALID;
        return VD_KERNEL_ERROR_HW_INVALID;
    }
    if(vd_armv7_encode_linked_context(context_id, &context) < 0)
    {
        result->failure_code = VD_KERNEL_ERROR_HW_INVALID;
        return VD_KERNEL_ERROR_HW_INVALID;
    }

    lock_engine();
    if(!__atomic_load_n(&engine_ready, __ATOMIC_SEQ_CST))
    {
        status = VD_KERNEL_ERROR_HW_DISABLED;
        goto finish;
    }
    if(hw_session.active)
    {
        status = VD_KERNEL_ERROR_HW_BUSY;
        goto finish;
    }

    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
    {
        core_state[i].inventory = (struct vd_kernel_hw_core_info){0};
        core_state[i].snapshot_valid = 0;
        core_state[i].monitor_changed = 0;
    }

    hw_session.pid = caller_pid;
    hw_session.context_id = context_id;
    hw_session.context = context;
    hw_session.token = next_hw_token++;
    if(hw_session.token == 0)
        hw_session.token = next_hw_token++;
    hw_session.deadline_us = (uint64_t)ksceKernelGetSystemTimeWide() +
                             (uint64_t)lease_ms * 1000u;
    hw_session.active = 1;

    status = dispatch_command(VD_HW_COMMAND_CLAIM, &failed_core);
    for(unsigned int i = 0; i < VD_KERNEL_HW_CORE_COUNT; ++i)
        copy_core_info(&result->core[i], &core_state[i].inventory);
    if(status < 0)
    {
        int original_status = status;
        int restore = restore_session_locked();
        if(restore < 0)
        {
            status = VD_KERNEL_ERROR_HW_RESTORE;
            result->token = hw_session.token;
        }
        else
        {
            status = original_status;
            if(original_status != VD_KERNEL_ERROR_HW_BUSY)
                __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
        }
        goto finish;
    }

    hw_session.deadline_us = (uint64_t)ksceKernelGetSystemTimeWide() +
                             (uint64_t)lease_ms * 1000u;
    __atomic_store_n(&engine_validated, 1, __ATOMIC_SEQ_CST);
    result->token = hw_session.token;
    status = 0;

finish:
    result->failure_core = failed_core;
    result->failure_code = status;
    unlock_engine();
    return status;
}

int vdHwDebugRenew(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int token,
    unsigned int lease_ms)
{
    int result = VD_KERNEL_ERROR_HW_OWNER;
    uint64_t now;
    if(caller_pid < 0 || context_id == 0 || token == 0 ||
       lease_ms < VD_HW_MIN_LEASE_MS || lease_ms > VD_HW_MAX_LEASE_MS)
        return VD_KERNEL_ERROR_HW_INVALID;

    lock_engine();
    now = (uint64_t)ksceKernelGetSystemTimeWide();
    if(hw_session.active && hw_session.pid == caller_pid &&
       hw_session.token == token && hw_session.context_id == context_id &&
       now < hw_session.deadline_us &&
       __atomic_load_n(&engine_ready, __ATOMIC_SEQ_CST))
    {
        hw_session.deadline_us = now + (uint64_t)lease_ms * 1000u;
        result = 0;
    }
    else if(hw_session.active && now >= hw_session.deadline_us)
    {
        if(restore_session_locked() < 0)
            result = VD_KERNEL_ERROR_HW_RESTORE;
    }
    else if(hw_session.active && hw_session.pid == caller_pid &&
            hw_session.token == token &&
            hw_session.context_id == context_id &&
            !__atomic_load_n(&engine_ready, __ATOMIC_SEQ_CST))
    {
        /* Do not let a lease keeper postpone cleanup of poisoned state. */
        result = VD_KERNEL_ERROR_HW_RESTORE;
    }
    unlock_engine();
    return result;
}

static int point_matches(const struct vd_hw_point* point,
                         const struct vd_kernel_hw_point_request* request)
{
    return point->active && point->type == request->type &&
           point->address == request->address &&
           point->length == request->length;
}

int vdHwDebugUpdate(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int token,
    const struct vd_kernel_hw_point_request* request)
{
    struct vd_hw_point* point;
    struct vd_armv7_debug_encoding encoding;
    int codec_result;
    int result;
    int failed_core = -1;
    uint64_t now;
    enum vd_armv7_watch_access access = VD_ARMV7_WATCH_ACCESS;

    if(caller_pid < 0 || context_id == 0 || token == 0 || !request ||
       request->size != sizeof(*request) || request->flags != 0)
        return VD_KERNEL_ERROR_HW_INVALID;
    if(request->operation != VD_KERNEL_HW_POINT_INSERT &&
       request->operation != VD_KERNEL_HW_POINT_REMOVE)
        return VD_KERNEL_ERROR_HW_INVALID;
    if(request->type == VD_KERNEL_HW_POINT_EXECUTE)
    {
        codec_result = vd_armv7_encode_linked_exec(
            request->address, request->length, &encoding);
    }
    else
    {
        if(request->type == VD_KERNEL_HW_POINT_READ)
            access = VD_ARMV7_WATCH_READ;
        else if(request->type == VD_KERNEL_HW_POINT_WRITE)
            access = VD_ARMV7_WATCH_WRITE;
        else if(request->type != VD_KERNEL_HW_POINT_ACCESS)
            return VD_KERNEL_ERROR_HW_INVALID;
        codec_result = vd_armv7_encode_linked_watch(
            request->address, request->length, access, &encoding);
    }
    if(codec_result < 0)
        return VD_KERNEL_ERROR_HW_INVALID;
    if(request->operation == VD_KERNEL_HW_POINT_INSERT &&
       (request->address + request->length < request->address ||
        ksceKernelFindProcMemBlockByAddr(
            caller_pid, (const void*)(uintptr_t)request->address,
            request->length) < 0))
        return VD_KERNEL_ERROR_HW_RANGE;

    lock_engine();
    now = (uint64_t)ksceKernelGetSystemTimeWide();
    if(!hw_session.active || hw_session.pid != caller_pid ||
       hw_session.token != token || hw_session.context_id != context_id ||
       now >= hw_session.deadline_us)
    {
        if(hw_session.active && now >= hw_session.deadline_us &&
           restore_session_locked() < 0)
        {
            unlock_engine();
            return VD_KERNEL_ERROR_HW_RESTORE;
        }
        unlock_engine();
        return VD_KERNEL_ERROR_HW_OWNER;
    }

    point = request->type == VD_KERNEL_HW_POINT_EXECUTE ?
        &hw_session.execute : &hw_session.watch;
    if(request->operation == VD_KERNEL_HW_POINT_INSERT)
    {
        if(point->active)
        {
            result = point_matches(point, request) ? 0 :
                VD_KERNEL_ERROR_HW_BUSY;
            unlock_engine();
            return result;
        }
    }
    else if(!point_matches(point, request))
    {
        unlock_engine();
        return 0;
    }

    if(request->operation == VD_KERNEL_HW_POINT_INSERT)
    {
        point->encoding = encoding;
        point->type = request->type;
        point->address = request->address;
        point->length = request->length;
        point->active = 1;
    }
    else
    {
        clear_point(point);
    }

    result = dispatch_command(VD_HW_COMMAND_APPLY, &failed_core);
    if(result < 0)
    {
        int original_result = result;
        /*
         * A timed-out worker may still be consuming the published session
         * payload.  Leave it immutable until restore_session_locked has first
         * quiesced that generation; teardown uses only the saved snapshots.
         */
        if(restore_session_locked() < 0)
            result = VD_KERNEL_ERROR_HW_RESTORE;
        else
        {
            result = original_result;
            __atomic_store_n(&engine_ready, 0, __ATOMIC_SEQ_CST);
        }
    }
    unlock_engine();
    return result;
}

int vdHwDebugRelease(SceUID caller_pid, unsigned int token)
{
    int result = VD_KERNEL_ERROR_HW_OWNER;
    lock_engine();
    if(hw_session.active && hw_session.pid == caller_pid &&
       hw_session.token == token)
        result = restore_session_locked();
    unlock_engine();
    return result;
}

#endif
