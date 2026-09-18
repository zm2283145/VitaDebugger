#include <stdint.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>

#include "vd_thread_setter_resolver_record.h"

/*
 * taiHEN deliberately exports this kernel-side module utility through
 * libtaihenModuleUtils_stub.a. Unlike the user-only taiGetModuleExportFunc(),
 * it accepts KERNEL_PID and does not use user-copy semantics. VitaShell and
 * kubridge use this exact declaration. This probe passes TAI_ANY_LIBRARY and
 * never calls the address it returns.
 */
int module_get_export_func(SceUID pid, const char* modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t* func);

#define VD_SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)

static const char* record_path(int slot)
{
    return slot == 0 ? VD_THREAD_SETTER_RESOLVER_RECORD_A_PATH :
                       VD_THREAD_SETTER_RESOLVER_RECORD_B_PATH;
}

static int read_slot(int slot,
                     struct vd_thread_setter_resolver_record* record)
{
    uint8_t extra;
    SceUID fd = ksceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    int result;
    int close_result;

    if(fd < 0)
        return fd;
    result = ksceIoRead(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
    {
        const int extra_result = ksceIoRead(fd, &extra, sizeof(extra));
        result = extra_result == 0 ? 0 :
            (extra_result < 0 ? extra_result :
                                VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);
    }
    else if(result >= 0)
    {
        result = VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
    }
    close_result = ksceIoClose(fd);
    if(result < 0)
        return result;
    if(close_result < 0)
        return close_result;
    return vd_thread_setter_resolver_record_valid(record) ? 0 :
        VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
}

static int slot_presence(int slot, int* present)
{
    SceIoStat stat;
    const int result = ksceIoGetstat(record_path(slot), &stat);

    if(result >= 0)
    {
        *present = 1;
        return 0;
    }
    if((uint32_t)result == VD_SCE_ERROR_ERRNO_ENOENT)
    {
        *present = 0;
        return 0;
    }
    *present = 1;
    return result;
}

static int read_latest(struct vd_thread_setter_resolver_record* record,
                       int* slot)
{
    struct vd_thread_setter_resolver_record a;
    struct vd_thread_setter_resolver_record b;
    int present_a = 1;
    int present_b = 1;
    const int presence_a_result = slot_presence(0, &present_a);
    const int presence_b_result = slot_presence(1, &present_b);
    if(presence_a_result < 0 || presence_b_result < 0)
        return VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
    const int valid_a = present_a && read_slot(0, &a) == 0;
    const int valid_b = present_b && read_slot(1, &b) == 0;
    const int selected = vd_thread_setter_resolver_select_latest(
        present_a, valid_a, valid_a ? a.revision : 0,
        present_b, valid_b, valid_b ? b.revision : 0);
    if(selected < 0)
        return selected;
    *record = selected == 0 ? a : b;
    *slot = selected;
    return 0;
}

static int write_slot_verified(
    int slot, struct vd_thread_setter_resolver_record* record)
{
    struct vd_thread_setter_resolver_record verify;
    SceUID fd;
    int result;
    int close_result;
    int sync_status = 0;

    record->magic = VD_THREAD_SETTER_RESOLVER_MAGIC;
    record->version = VD_THREAD_SETTER_RESOLVER_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_thread_setter_resolver_checksum(record);

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
                      struct vd_thread_setter_resolver_record* record,
                      int* written_slot)
{
    const int target_slot = previous_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);

    if(result >= 0)
        *written_slot = target_slot;
    return result;
}

static void copy_module_name(char destination[28], const char source[27])
{
    uint32_t i;
    for(i = 0; i < 27; ++i)
    {
        destination[i] = source[i];
        if(source[i] == '\0')
            break;
    }
    for(; i < 28; ++i)
        destination[i] = '\0';
    destination[27] = '\0';
}

static void capture_firmware(
    struct vd_thread_setter_resolver_record* record)
{
    SceKernelFwInfo firmware = {0};
    firmware.size = sizeof(firmware);
    record->firmware_result = ksceKernelGetSystemSwVersion(&firmware);
    if(record->firmware_result >= 0)
        record->firmware_version = firmware.version;
}

static void capture_module(
    struct vd_thread_setter_resolver_record* record)
{
    tai_module_info_t module = {0};
    module.size = sizeof(module);
    record->module_lookup_result = taiGetModuleInfoForKernel(
        KERNEL_PID, VD_THREAD_SETTER_RESOLVER_MODULE, &module);
    if(record->module_lookup_result >= 0)
    {
        record->module_id = (int32_t)module.modid;
        record->module_nid = module.module_nid;
        record->exports_start = (uint32_t)module.exports_start;
        record->exports_end = (uint32_t)module.exports_end;
        copy_module_name(record->module_name, module.name);
    }
}

static void capture_target(
    struct vd_thread_setter_resolver_target* target)
{
    uintptr_t address = 0;

    target->lookup_result = module_get_export_func(
        KERNEL_PID, VD_THREAD_SETTER_RESOLVER_MODULE, TAI_ANY_LIBRARY,
        target->nid, &address);
    if(target->lookup_result < 0)
        return;
    if(address == 0)
    {
        target->lookup_result = VD_THREAD_SETTER_RESOLVER_EMPTY_ADDRESS;
        return;
    }

    target->raw_address = (uint32_t)address;
    target->code_address = target->raw_address & ~UINT32_C(1);
    target->flags = VD_THREAD_SETTER_TARGET_FLAG_RESOLVED;
    if((target->raw_address & UINT32_C(1)) != 0)
        target->flags |= VD_THREAD_SETTER_TARGET_FLAG_THUMB;
}

static void count_resolved_targets(
    struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    record->resolved_count = 0;
    for(i = 0; i < record->completed_target_count; ++i)
        if((record->targets[i].flags &
            VD_THREAD_SETTER_TARGET_FLAG_RESOLVED) != 0)
            record->resolved_count++;
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    struct vd_thread_setter_resolver_record record;
    int current_slot;

    (void)args;
    (void)argp;
    if(read_latest(&record, &current_slot) < 0 ||
       record.state != VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED ||
       !vd_thread_setter_resolver_attempt_payload_valid(&record) ||
       record.sequence == 0 || record.revision > UINT32_MAX - 8u)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.revision++;
    record.state = VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    capture_firmware(&record);
    record.revision++;
    record.state = VD_THREAD_SETTER_RESOLVER_STATE_FIRMWARE_RECORDED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    capture_module(&record);
    record.revision++;
    record.state = VD_THREAD_SETTER_RESOLVER_STATE_MODULE_RECORDED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    for(uint32_t i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        capture_target(&record.targets[i]);
        record.completed_target_count = i + 1u;
        count_resolved_targets(&record);
        record.revision++;
        record.state = VD_THREAD_SETTER_RESOLVER_STATE_TARGET_RECORDED;
        if(write_next(current_slot, &record, &current_slot) < 0)
            return SCE_KERNEL_START_NO_RESIDENT;
    }

    vd_thread_setter_resolver_finalize(&record);
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
