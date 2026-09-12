#include <stdint.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/dipsw.h>
#include <psp2kern/kernel/modulemgr.h>

#include "vd_dipsw_probe_record.h"

static const char* record_path(int slot)
{
    return slot == 0 ? VD_DIPSW_PROBE_RECORD_A_PATH :
                       VD_DIPSW_PROBE_RECORD_B_PATH;
}

static int read_slot(int slot, struct vd_dipsw_probe_record* record)
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
    return vd_dipsw_probe_record_valid(record) ? 0 : -2;
}

static int read_latest(struct vd_dipsw_probe_record* record, int* slot)
{
    struct vd_dipsw_probe_record a;
    struct vd_dipsw_probe_record b;
    const int have_a = read_slot(0, &a) == 0;
    const int have_b = read_slot(1, &b) == 0;

    if(!have_a && !have_b)
        return -1;
    if(have_a && have_b && a.revision == b.revision)
        return -3;
    if(have_b && (!have_a ||
                  vd_dipsw_probe_revision_newer(b.revision, a.revision)))
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
                               struct vd_dipsw_probe_record* record)
{
    struct vd_dipsw_probe_record verify;
    SceUID fd;
    int result;
    int close_result;
    int sync_status = 0;

    record->magic = VD_DIPSW_PROBE_MAGIC;
    record->version = VD_DIPSW_PROBE_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_dipsw_probe_checksum(record);

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
                      struct vd_dipsw_probe_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static __attribute__((noinline)) void collect_read_only_state(
    struct vd_dipsw_probe_record* record)
{
    record->cp_build_version = ksceKernelGetDipswInfo(1);
    record->debug_control =
        ksceKernelGetDipswInfo(VD_DIPSW_DEBUG_INFO_INDEX);
    record->system_control =
        ksceKernelGetDipswInfo(VD_DIPSW_SYSTEM_INFO_INDEX);
    record->check_203 = ksceKernelCheckDipsw(VD_DIPSW_RECONFIG_BIT);
    record->check_228 = ksceKernelCheckDipsw(VD_DIPSW_HW_DEBUG_BIT);
    vd_dipsw_probe_validate_readback(record);
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    struct vd_dipsw_probe_record record;
    int current_slot;

    (void)args;
    (void)argp;
    if(read_latest(&record, &current_slot) < 0 ||
       record.state != VD_DIPSW_PROBE_STATE_ATTEMPTED ||
       record.result != VD_DIPSW_PROBE_NOT_RUN ||
       record.flags != 0 ||
       record.sequence == 0 ||
       record.revision > UINT32_MAX - 2u)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.revision++;
    record.state = VD_DIPSW_PROBE_STATE_KERNEL_ENTERED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.core_id = (uint32_t)ksceKernelCpuId();
    collect_read_only_state(&record);
    record.state = VD_DIPSW_PROBE_STATE_COMPLETE;
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
