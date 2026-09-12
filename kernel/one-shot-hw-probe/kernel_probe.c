#include <stdint.h>

#include <psp2kern/io/dirent.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>

#include "vd_armv7_debug_codec.h"
#include "vd_hw_probe_record.h"

#define VD_HW_PROBE_ENABLE UINT32_C(1)
#define VD_HW_PROBE_DSCR_HDBGEN (UINT32_C(1) << 14)
#define VD_HW_PROBE_DSCR_MDBGEN (UINT32_C(1) << 15)
#define VD_HW_PROBE_DSCR_ACTIVE_MASK \
    (VD_HW_PROBE_DSCR_HDBGEN | VD_HW_PROBE_DSCR_MDBGEN)
#define VD_HW_PROBE_BCR_COMPARE_MASK UINT32_C(0x007fe1e7)
#define VD_HW_PROBE_WCR_COMPARE_MASK UINT32_C(0x1f1fe1ff)
#define VD_HW_PROBE_VALUE_COMPARE_MASK UINT32_C(0xfffffffc)
#define VD_HW_PROBE_MIDR_MASK UINT32_C(0xff0ffff0)
#define VD_HW_PROBE_CORTEX_A9_MIDR UINT32_C(0x410fc090)
#define VD_HW_PROBE_BREAK_TEST_ADDRESS UINT32_C(0x81234560)
#define VD_HW_PROBE_WATCH_TEST_ADDRESS UINT32_C(0x856789a0)

static struct vd_hw_probe_record probe_record;

static int write_record(void)
{
    probe_record.magic = VD_HW_PROBE_MAGIC;
    probe_record.version = VD_HW_PROBE_VERSION;
    probe_record.size = (uint32_t)sizeof(probe_record);
    probe_record.checksum = 0;
    probe_record.checksum = vd_hw_probe_record_checksum(&probe_record);

    ksceIoRemove(VD_HW_PROBE_TEMP_PATH);
    SceUID fd = ksceIoOpen(VD_HW_PROBE_TEMP_PATH,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if(fd < 0)
        return fd;

    int result = ksceIoWrite(fd, &probe_record, sizeof(probe_record));
    if(result == (int)sizeof(probe_record))
    {
        int sync_status = 0;
        int sync_result = ksceIoSyncByFd(fd, &sync_status);
        if(sync_result < 0)
            result = sync_result;
        else if(sync_status < 0)
            result = sync_status;
        else
            result = 0;
    }
    else if(result >= 0)
    {
        result = -1;
    }

    int close_result = ksceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result < 0)
        return result;

    ksceIoRemove(VD_HW_PROBE_RESULT_PATH);
    result = ksceIoRename(VD_HW_PROBE_TEMP_PATH,
                          VD_HW_PROBE_RESULT_PATH);
    if(result >= 0)
        result = ksceIoSync("ux0:", 0);
    return result;
}

static void debug_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static uint32_t read_midr(void)
{
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(value));
    return value;
}

static uint32_t read_didr(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 0" : "=r"(value));
    return value;
}

static uint32_t read_dscr(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c1, 0" : "=r"(value));
    return value;
}

static uint32_t read_dbgvcr(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c7, 0" : "=r"(value));
    return value;
}

static uint32_t read_bvr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 4" : "=r"(value));
    return value;
}

static uint32_t read_bcr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 5" : "=r"(value));
    return value;
}

static uint32_t read_wvr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 6" : "=r"(value));
    return value;
}

static uint32_t read_wcr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 7" : "=r"(value));
    return value;
}

static void write_bvr0(uint32_t value)
{
    __asm__ volatile("mcr p14, 0, %0, c0, c0, 4" : : "r"(value) : "memory");
}

static void write_bcr0(uint32_t value)
{
    __asm__ volatile("mcr p14, 0, %0, c0, c0, 5" : : "r"(value) : "memory");
}

static void write_wvr0(uint32_t value)
{
    __asm__ volatile("mcr p14, 0, %0, c0, c0, 6" : : "r"(value) : "memory");
}

static void write_wcr0(uint32_t value)
{
    __asm__ volatile("mcr p14, 0, %0, c0, c0, 7" : : "r"(value) : "memory");
}

static int masked_equal(uint32_t actual, uint32_t expected, uint32_t mask)
{
    return ((actual ^ expected) & mask) == 0;
}

static int restore_snapshot(void)
{
    probe_record.flags |= VD_HW_PROBE_FLAG_RESTORE_ATTEMPTED;

    /* Both controls were proven disabled during acquisition. */
    write_bcr0(probe_record.original_bcr0 & ~VD_HW_PROBE_ENABLE);
    write_wcr0(probe_record.original_wcr0 & ~VD_HW_PROBE_ENABLE);
    debug_isb();
    write_bvr0(probe_record.original_bvr0);
    write_wvr0(probe_record.original_wvr0);
    debug_isb();
    write_bcr0(probe_record.original_bcr0);
    write_wcr0(probe_record.original_wcr0);
    debug_isb();

    probe_record.restored_bvr0 = read_bvr0();
    probe_record.restored_bcr0 = read_bcr0();
    probe_record.restored_wvr0 = read_wvr0();
    probe_record.restored_wcr0 = read_wcr0();

    if(!masked_equal(probe_record.restored_bvr0,
                     probe_record.original_bvr0,
                     VD_HW_PROBE_VALUE_COMPARE_MASK) ||
       !masked_equal(probe_record.restored_bcr0,
                     probe_record.original_bcr0,
                     VD_HW_PROBE_BCR_COMPARE_MASK))
        return VD_HW_PROBE_ERROR_BREAK_RESTORE;
    if(!masked_equal(probe_record.restored_wvr0,
                     probe_record.original_wvr0,
                     VD_HW_PROBE_VALUE_COMPARE_MASK) ||
       !masked_equal(probe_record.restored_wcr0,
                     probe_record.original_wcr0,
                     VD_HW_PROBE_WCR_COMPARE_MASK))
        return VD_HW_PROBE_ERROR_WATCH_RESTORE;

    probe_record.flags |= VD_HW_PROBE_FLAG_RESTORE_VERIFIED;
    probe_record.stage = VD_HW_PROBE_STAGE_RESTORED;
    return VD_HW_PROBE_OK;
}

static int run_disabled_roundtrip(void)
{
    struct vd_armv7_debug_encoding execute_encoding;
    struct vd_armv7_debug_encoding watch_encoding;
    int result = VD_HW_PROBE_OK;
    int restore_result;
    SceKernelIntrStatus interrupt_state = ksceKernelCpuSuspendIntr();

    probe_record.core_id = (uint32_t)ksceKernelCpuId();
    if(probe_record.core_id > 2u)
    {
        result = VD_HW_PROBE_ERROR_WRONG_CORE;
        goto finish;
    }

    probe_record.raw_midr = read_midr();
    if((probe_record.raw_midr & VD_HW_PROBE_MIDR_MASK) !=
       VD_HW_PROBE_CORTEX_A9_MIDR)
    {
        result = VD_HW_PROBE_ERROR_WRONG_CPU;
        goto finish;
    }

    probe_record.raw_didr = read_didr();
    probe_record.raw_dscr = read_dscr();
    probe_record.raw_dbgvcr = read_dbgvcr();
    probe_record.breakpoint_count =
        ((probe_record.raw_didr >> 24) & UINT32_C(0xf)) + 1u;
    probe_record.watchpoint_count =
        ((probe_record.raw_didr >> 28) & UINT32_C(0xf)) + 1u;
    probe_record.context_breakpoint_count =
        ((probe_record.raw_didr >> 20) & UINT32_C(0xf)) + 1u;
    if(probe_record.breakpoint_count < 6u ||
       probe_record.watchpoint_count < 4u ||
       probe_record.context_breakpoint_count < 2u)
    {
        result = VD_HW_PROBE_ERROR_NO_COMPARATORS;
        goto finish;
    }
    if((probe_record.raw_dscr & VD_HW_PROBE_DSCR_ACTIVE_MASK) != 0)
    {
        result = VD_HW_PROBE_ERROR_DEBUG_ACTIVE;
        goto finish;
    }
    if(probe_record.raw_dbgvcr != 0)
    {
        result = VD_HW_PROBE_ERROR_VECTOR_CATCH_ACTIVE;
        goto finish;
    }

    probe_record.original_bvr0 = read_bvr0();
    probe_record.original_bcr0 = read_bcr0();
    probe_record.original_wvr0 = read_wvr0();
    probe_record.original_wcr0 = read_wcr0();
    if((probe_record.original_bcr0 & VD_HW_PROBE_ENABLE) != 0 ||
       (probe_record.original_wcr0 & VD_HW_PROBE_ENABLE) != 0)
    {
        result = VD_HW_PROBE_ERROR_COMPARATOR_BUSY;
        goto finish;
    }
    probe_record.flags |= VD_HW_PROBE_FLAG_SNAPSHOT_VALID;
    probe_record.stage = VD_HW_PROBE_STAGE_SNAPSHOT;

    if(vd_armv7_encode_linked_exec(VD_HW_PROBE_BREAK_TEST_ADDRESS, 4,
                                    &execute_encoding) < 0 ||
       vd_armv7_encode_linked_watch(VD_HW_PROBE_WATCH_TEST_ADDRESS, 4,
                                    VD_ARMV7_WATCH_WRITE,
                                    &watch_encoding) < 0)
    {
        result = VD_HW_PROBE_ERROR_CODEC;
        goto restore;
    }

    /* The enable bit is cleared before any value reaches CP14. */
    execute_encoding.control &= ~VD_HW_PROBE_ENABLE;
    watch_encoding.control &= ~VD_HW_PROBE_ENABLE;
    probe_record.test_bvr0 = execute_encoding.value;
    probe_record.test_bcr0 = execute_encoding.control;
    probe_record.test_wvr0 = watch_encoding.value;
    probe_record.test_wcr0 = watch_encoding.control;

    write_bcr0(probe_record.test_bcr0);
    probe_record.flags |= VD_HW_PROBE_FLAG_BREAK_CONTROL_WRITTEN;
    write_wcr0(probe_record.test_wcr0);
    probe_record.flags |= VD_HW_PROBE_FLAG_WATCH_CONTROL_WRITTEN;
    debug_isb();
    probe_record.readback_bcr0 = read_bcr0();
    probe_record.readback_wcr0 = read_wcr0();
    if(!masked_equal(probe_record.readback_bcr0,
                     probe_record.test_bcr0,
                     VD_HW_PROBE_BCR_COMPARE_MASK))
    {
        result = VD_HW_PROBE_ERROR_BREAK_CONTROL_READBACK;
        goto restore;
    }
    if(!masked_equal(probe_record.readback_wcr0,
                     probe_record.test_wcr0,
                     VD_HW_PROBE_WCR_COMPARE_MASK))
    {
        result = VD_HW_PROBE_ERROR_WATCH_CONTROL_READBACK;
        goto restore;
    }

    write_bvr0(probe_record.test_bvr0);
    probe_record.flags |= VD_HW_PROBE_FLAG_BREAK_VALUE_WRITTEN;
    write_wvr0(probe_record.test_wvr0);
    probe_record.flags |= VD_HW_PROBE_FLAG_WATCH_VALUE_WRITTEN;
    debug_isb();
    probe_record.stage = VD_HW_PROBE_STAGE_TEST_WRITTEN;
    probe_record.readback_bvr0 = read_bvr0();
    probe_record.readback_wvr0 = read_wvr0();
    if(!masked_equal(probe_record.readback_bvr0,
                     probe_record.test_bvr0,
                     VD_HW_PROBE_VALUE_COMPARE_MASK))
    {
        result = VD_HW_PROBE_ERROR_BREAK_VALUE_READBACK;
        goto restore;
    }
    if(!masked_equal(probe_record.readback_wvr0,
                     probe_record.test_wvr0,
                     VD_HW_PROBE_VALUE_COMPARE_MASK))
    {
        result = VD_HW_PROBE_ERROR_WATCH_VALUE_READBACK;
        goto restore;
    }

restore:
    restore_result = restore_snapshot();
    if(restore_result < 0)
        result = restore_result;

finish:
    ksceKernelCpuResumeIntr(interrupt_state);
    return result;
}

static void initialize_record(void)
{
    memset(&probe_record, 0, sizeof(probe_record));
    probe_record.magic = VD_HW_PROBE_MAGIC;
    probe_record.version = VD_HW_PROBE_VERSION;
    probe_record.size = (uint32_t)sizeof(probe_record);
    probe_record.result = VD_HW_PROBE_NOT_RUN;
    probe_record.stage = VD_HW_PROBE_STAGE_ENTERED;
    probe_record.core_id = UINT32_MAX;
    probe_record.initial_journal_result = -1;
    probe_record.final_journal_result = -1;
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    initialize_record();
    probe_record.initial_journal_result = write_record();
    if(probe_record.initial_journal_result < 0)
    {
        /* Never enter CP14 without a durable pre-probe marker. */
        probe_record.result = VD_HW_PROBE_ERROR_JOURNAL;
        probe_record.stage = VD_HW_PROBE_STAGE_COMPLETE;
        probe_record.final_journal_result = 0;
        (void)write_record();
        return SCE_KERNEL_START_NO_RESIDENT;
    }
    probe_record.result = run_disabled_roundtrip();
    probe_record.flags |= VD_HW_PROBE_FLAG_AUTO_UNLOAD_SAFE;
    probe_record.stage = VD_HW_PROBE_STAGE_COMPLETE;
    probe_record.final_journal_result = 0;
    (void)write_record();
    return SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_SUCCESS;
}
