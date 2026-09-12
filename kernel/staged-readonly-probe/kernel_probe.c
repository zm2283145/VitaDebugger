#include <stdint.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>

#include "vd_read_ladder_record.h"

#define VD_READ_LIFECYCLE_VALUE UINT32_C(0x4c494645)

static const char* record_path(int slot)
{
    return slot == 0 ? VD_READ_LADDER_RECORD_A_PATH :
                       VD_READ_LADDER_RECORD_B_PATH;
}

static int read_slot(int slot, struct vd_read_ladder_record* record)
{
    SceUID fd = ksceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    int result;
    int close_result;

    if(fd < 0)
        return fd;
    result = ksceIoRead(fd, record, sizeof(*record));
    close_result = ksceIoClose(fd);
    if(result != (int)sizeof(*record))
        return result < 0 ? result : -1;
    if(close_result < 0)
        return close_result;
    return vd_read_ladder_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_read_ladder_record* record, int* slot)
{
    struct vd_read_ladder_record a;
    struct vd_read_ladder_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_b && (!have_a ||
                  vd_read_ladder_revision_newer(b.revision, a.revision)))
    {
        *record = b;
        *slot = 1;
    }
    else
    {
        *record = a;
        *slot = 0;
    }
    return 0;
}

static int write_slot_verified(int slot,
                               struct vd_read_ladder_record* record)
{
    struct vd_read_ladder_record verify;
    SceUID fd;
    int result;
    int close_result;
    int sync_status = 0;

    record->magic = VD_READ_LADDER_MAGIC;
    record->version = VD_READ_LADDER_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_read_ladder_checksum(record);

    fd = ksceIoOpen(record_path(slot),
                    SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if(fd < 0)
        return fd;
    result = ksceIoWrite(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
    {
        const int sync_result = ksceIoSyncByFd(fd, &sync_status);
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
    close_result = ksceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result >= 0)
        result = ksceIoSync("ux0:", 0);
    if(result < 0)
        return result;
    if(read_slot(slot, &verify) < 0 ||
       verify.revision != record->revision ||
       verify.sequence != record->sequence ||
       verify.step != record->step ||
       verify.state != record->state ||
       verify.checksum != record->checksum)
        return -3;
    return 0;
}

static int write_next(int previous_slot,
                      struct vd_read_ladder_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
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

static uint32_t read_bcr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 5" : "=r"(value));
    return value;
}

static uint32_t read_bvr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 4" : "=r"(value));
    return value;
}

static uint32_t read_wcr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 7" : "=r"(value));
    return value;
}

static uint32_t read_wvr0(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 6" : "=r"(value));
    return value;
}

static int execute_one_step(uint32_t step, uint32_t* value,
                            uint32_t* flags)
{
    switch(step)
    {
        case VD_READ_STEP_KERNEL_LIFECYCLE:
            *value = VD_READ_LIFECYCLE_VALUE;
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_CPU_ID:
            *value = (uint32_t)ksceKernelCpuId();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_MIDR:
            *value = read_midr();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_DIDR:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_didr();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_DSCR:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_dscr();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_DBGVCR:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_dbgvcr();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_BCR0:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_bcr0();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_BVR0:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_bvr0();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_WCR0:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_wcr0();
            return VD_READ_RESULT_OK;
        case VD_READ_STEP_WVR0:
            *flags |= VD_READ_FLAG_CP14;
            *value = read_wvr0();
            return VD_READ_RESULT_OK;
        default:
            return VD_READ_RESULT_BAD_STEP;
    }
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    struct vd_read_ladder_record record;
    SceKernelIntrStatus interrupt_state;
    uint32_t active_core;
    int current_slot;
    int result;

    (void)args;
    (void)argp;
    if(read_latest(&record, &current_slot) < 0 ||
       record.state != VD_READ_STATE_ATTEMPTED ||
       record.result != VD_READ_RESULT_NOT_RUN ||
       record.step < VD_READ_STEP_FIRST ||
       record.step > VD_READ_STEP_LAST ||
       record.revision > UINT32_MAX - 2u)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.revision++;
    record.state = VD_READ_STATE_KERNEL_ENTERED;
    record.value = 0;
    record.flags = VD_READ_FLAG_GENERAL_REGS_ONLY;
    record.core_id = (uint32_t)ksceKernelCpuId();
    if(record.step >= VD_READ_STEP_DIDR)
        record.flags |= VD_READ_FLAG_CP14;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    /*
     * The durable entry above identifies the selected operation and the core
     * on which it is expected to run. Interrupts are disabled only across the
     * core recheck and that single operation, preventing scheduler migration.
     */
    interrupt_state = ksceKernelCpuSuspendIntr();
    record.flags |= VD_READ_FLAG_IRQ_GUARDED;
    active_core = (uint32_t)ksceKernelCpuId();
    if(active_core != record.core_id)
    {
        record.value = active_core;
        record.flags |= VD_READ_FLAG_VALUE_VALID;
        result = VD_READ_RESULT_CORE_MIGRATED;
    }
    else if(active_core > 2u)
    {
        record.value = active_core;
        record.flags |= VD_READ_FLAG_CORE_MATCH |
                        VD_READ_FLAG_VALUE_VALID;
        result = VD_READ_RESULT_UNSAFE_CORE;
    }
    else
    {
        record.flags |= VD_READ_FLAG_CORE_MATCH;
        result = execute_one_step(record.step, &record.value, &record.flags);
    }
    ksceKernelCpuResumeIntr(interrupt_state);
    record.result = result;
    if(result == VD_READ_RESULT_OK)
        record.flags |= VD_READ_FLAG_VALUE_VALID;
    record.state = VD_READ_STATE_COMPLETE;
    record.revision++;
    (void)write_next(current_slot, &record, &current_slot);
    return SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_SUCCESS;
}
