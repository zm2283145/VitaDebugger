#include <stdint.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/dipsw.h>
#include <psp2kern/kernel/modulemgr.h>

#include "vd_dipsw_dbgvcr_record.h"

static const char* record_path(int slot)
{
    return slot == 0 ? VD_DIPSW_DBGVCR_RECORD_A_PATH :
                       VD_DIPSW_DBGVCR_RECORD_B_PATH;
}

static int read_slot(int slot, struct vd_dipsw_dbgvcr_record* record)
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
    return vd_dipsw_dbgvcr_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_dipsw_dbgvcr_record* record, int* slot)
{
    struct vd_dipsw_dbgvcr_record a;
    struct vd_dipsw_dbgvcr_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_a && have_b && a.revision == b.revision)
        return -3;
    if(have_b && (!have_a ||
                  vd_dipsw_dbgvcr_revision_newer(b.revision, a.revision)))
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
                               struct vd_dipsw_dbgvcr_record* record)
{
    struct vd_dipsw_dbgvcr_record verify;
    SceUID fd;
    int result;
    int close_result;
    int sync_status = 0;

    record->magic = VD_DIPSW_DBGVCR_MAGIC;
    record->version = VD_DIPSW_DBGVCR_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_dipsw_dbgvcr_checksum(record);

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
       verify.state != record->state ||
       verify.checksum != record->checksum)
        return -3;
    return 0;
}

static int write_next(int previous_slot,
                      struct vd_dipsw_dbgvcr_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void capture_before(struct vd_dipsw_dbgvcr_record* record)
{
    record->before_cp_build_version = ksceKernelGetDipswInfo(1);
    record->before_debug =
        ksceKernelGetDipswInfo(VD_DIPSW_DEBUG_INFO_INDEX);
    record->before_system =
        ksceKernelGetDipswInfo(VD_DIPSW_SYSTEM_INFO_INDEX);
    record->before_check_203 =
        ksceKernelCheckDipsw(VD_DIPSW_RECONFIG_BIT);
    record->before_check_228 =
        ksceKernelCheckDipsw(VD_DIPSW_HW_DEBUG_BIT);
    vd_dipsw_dbgvcr_validate_before(record);
}

static void capture_confirm(struct vd_dipsw_dbgvcr_record* record)
{
    record->confirm_cp_build_version = ksceKernelGetDipswInfo(1);
    record->confirm_debug =
        ksceKernelGetDipswInfo(VD_DIPSW_DEBUG_INFO_INDEX);
    record->confirm_system =
        ksceKernelGetDipswInfo(VD_DIPSW_SYSTEM_INFO_INDEX);
    record->confirm_check_203 =
        ksceKernelCheckDipsw(VD_DIPSW_RECONFIG_BIT);
    record->confirm_check_228 =
        ksceKernelCheckDipsw(VD_DIPSW_HW_DEBUG_BIT);
    vd_dipsw_dbgvcr_validate_confirm(record);
}

static void capture_after_restore(struct vd_dipsw_dbgvcr_record* record)
{
    record->after_restore_cp_build_version = ksceKernelGetDipswInfo(1);
    record->after_restore_debug =
        ksceKernelGetDipswInfo(VD_DIPSW_DEBUG_INFO_INDEX);
    record->after_restore_system =
        ksceKernelGetDipswInfo(VD_DIPSW_SYSTEM_INFO_INDEX);
    record->after_restore_check_203 =
        ksceKernelCheckDipsw(VD_DIPSW_RECONFIG_BIT);
    record->after_restore_check_228 =
        ksceKernelCheckDipsw(VD_DIPSW_HW_DEBUG_BIT);
}

static __attribute__((noinline)) uint32_t read_dbgvcr_once(void)
{
    uint32_t value;
    __asm__ volatile("mrc p14, 0, %0, c0, c7, 0" : "=r"(value));
    return value;
}

static void write_complete(struct vd_dipsw_dbgvcr_record* record,
                           int* current_slot)
{
    record->revision++;
    record->state = VD_DIPSW_DBGVCR_STATE_COMPLETE;
    (void)write_next(*current_slot, record, current_slot);
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    struct vd_dipsw_dbgvcr_record record;
    SceKernelIntrStatus interrupt_state;
    int current_slot;
    int journal_result;

    (void)args;
    (void)argp;
    if(read_latest(&record, &current_slot) < 0 ||
       record.state != VD_DIPSW_DBGVCR_STATE_ATTEMPTED ||
       record.result != VD_DIPSW_DBGVCR_NOT_RUN ||
       record.restore_result != VD_DIPSW_DBGVCR_NOT_RUN ||
       record.flags != 0 ||
       record.sequence == 0 ||
       record.hazard_armed != 0 ||
       record.set_call_count != 0 ||
       record.dbgvcr_read_count != 0 ||
       record.clear_call_count != 0 ||
       record.journal_error != 0 ||
       record.revision > UINT32_MAX - 4u)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.revision++;
    record.state = VD_DIPSW_DBGVCR_STATE_KERNEL_ENTERED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    capture_before(&record);
    if(record.result != VD_DIPSW_DBGVCR_OK)
    {
        write_complete(&record, &current_slot);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    record.revision++;
    record.state = VD_DIPSW_DBGVCR_STATE_ORIGINAL_CAPTURED;
    journal_result = write_next(current_slot, &record, &current_slot);
    if(journal_result < 0)
    {
        record.journal_error = journal_result;
        record.result = VD_DIPSW_DBGVCR_ERROR_JOURNAL_ORIGINAL;
        write_complete(&record, &current_slot);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    record.core_before = (uint32_t)ksceKernelCpuId();
    record.hazard_armed = 1;
    record.revision++;
    record.state = VD_DIPSW_DBGVCR_STATE_READ_PENDING;
    journal_result = write_next(current_slot, &record, &current_slot);
    if(journal_result < 0)
    {
        record.hazard_armed = 0;
        record.journal_error = journal_result;
        record.result = VD_DIPSW_DBGVCR_ERROR_JOURNAL_READ_PENDING;
        write_complete(&record, &current_slot);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    capture_confirm(&record);
    if(record.result != VD_DIPSW_DBGVCR_OK)
    {
        write_complete(&record, &current_slot);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    /*
     * No file, UI, allocation, delay, hook, or thread call occurs while bit
     * 228 is high. Clear is reached on every returning path after Set. The
     * single CP14 read can still reset or hang unsupported retail hardware.
     */
    record.set_call_count = 1;
    ksceKernelSetDipsw(VD_DIPSW_HW_DEBUG_BIT);
    record.after_set_system =
        ksceKernelGetDipswInfo(VD_DIPSW_SYSTEM_INFO_INDEX);
    record.after_set_check_228 =
        ksceKernelCheckDipsw(VD_DIPSW_HW_DEBUG_BIT);
    vd_dipsw_dbgvcr_validate_after_set(&record);

    if(record.result == VD_DIPSW_DBGVCR_OK)
    {
        interrupt_state = ksceKernelCpuSuspendIntr();
        record.flags |= VD_DIPSW_DBGVCR_FLAG_IRQ_GUARDED;
        record.core_read = (uint32_t)ksceKernelCpuId();
        if(record.core_read != record.core_before)
        {
            record.result = VD_DIPSW_DBGVCR_ERROR_CORE_MIGRATED;
        }
        else if(record.core_read > 2u)
        {
            record.flags |= VD_DIPSW_DBGVCR_FLAG_CORE_MATCH;
            record.result = VD_DIPSW_DBGVCR_ERROR_UNSAFE_CORE;
        }
        else
        {
            record.flags |= VD_DIPSW_DBGVCR_FLAG_CORE_MATCH |
                            VD_DIPSW_DBGVCR_FLAG_CORE_ALLOWED;
            record.dbgvcr_read_count = 1;
            record.dbgvcr_value = read_dbgvcr_once();
            record.flags |= VD_DIPSW_DBGVCR_FLAG_READ_RETURNED;
        }
        ksceKernelCpuResumeIntr(interrupt_state);
    }

    record.clear_call_count = 1;
    ksceKernelClearDipsw(VD_DIPSW_HW_DEBUG_BIT);
    capture_after_restore(&record);
    vd_dipsw_dbgvcr_validate_after_restore(&record);
    write_complete(&record, &current_slot);
    return SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_SUCCESS;
}
