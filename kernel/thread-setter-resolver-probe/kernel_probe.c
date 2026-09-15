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

static void capture_segments(
    struct vd_thread_setter_resolver_record* record,
    const SceKernelModuleInfo* module)
{
    uint32_t i;
    record->segment_count = 0;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT; ++i)
    {
        struct vd_thread_setter_resolver_segment* destination =
            &record->segments[i];
        const SceKernelSegmentInfo* source = &module->segments[i];
        destination->base = (uint32_t)(uintptr_t)source->vaddr;
        destination->memsz = (uint32_t)source->memsz;
        destination->filesz = (uint32_t)source->filesz;
        destination->permissions = (uint32_t)source->perms;
        if(destination->base != 0 || destination->memsz != 0 ||
           destination->filesz != 0 || destination->permissions != 0)
            record->segment_count++;
    }
}

static int locate_code_segment(
    const struct vd_thread_setter_resolver_record* record,
    uint32_t code_address, uint32_t* segment_index,
    uint32_t* segment_offset)
{
    uint32_t i;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_segment* segment =
            &record->segments[i];
        if(vd_thread_setter_resolver_range_within(
               code_address, 1, segment->base, segment->memsz))
        {
            *segment_index = i;
            *segment_offset = code_address - segment->base;
            return 1;
        }
    }
    return 0;
}

static void capture_target(
    struct vd_thread_setter_resolver_record* record,
    struct vd_thread_setter_resolver_target* target)
{
    uintptr_t address = 0;
    uint32_t segment_index;
    uint32_t segment_offset;
    uint32_t i;

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
    if(!locate_code_segment(record, target->code_address, &segment_index,
                            &segment_offset))
        return;

    target->segment_index = (int32_t)segment_index;
    target->segment_offset = segment_offset;
    target->flags |= VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT;
    const struct vd_thread_setter_resolver_segment* segment =
        &record->segments[segment_index];
    if((segment->permissions &
        VD_THREAD_SETTER_RESOLVER_EXECUTE_PERMISSION) == 0)
        return;
    target->flags |= VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE;
    if(!vd_thread_setter_resolver_range_within(
           target->code_address, VD_THREAD_SETTER_RESOLVER_CODE_BYTES,
           segment->base, segment->memsz))
        return;
    target->flags |= VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED;

    /* The source was proven to be a fixed-size window in an executable module
     * segment. A volatile byte loop prevents an implementation from widening
     * the read beyond the recorded 64-byte disclosure boundary. */
    const volatile uint8_t* source =
        (const volatile uint8_t*)(uintptr_t)target->code_address;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_CODE_BYTES; ++i)
        target->code[i] = source[i];
    target->code_size = VD_THREAD_SETTER_RESOLVER_CODE_BYTES;
    target->flags |= VD_THREAD_SETTER_TARGET_FLAG_CAPTURED;
}

static void collect_read_only_state(
    struct vd_thread_setter_resolver_record* record)
{
    SceKernelFwInfo firmware = {0};
    tai_module_info_t tai_module = {0};
    SceKernelModuleInfo module = {0};
    uint32_t i;

    firmware.size = sizeof(firmware);
    record->firmware_result = ksceKernelGetSystemSwVersion(&firmware);
    if(record->firmware_result >= 0)
    {
        record->firmware_version = firmware.version;
        record->flags |=
            VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK;
    }

    tai_module.size = sizeof(tai_module);
    record->module_lookup_result = taiGetModuleInfoForKernel(
        KERNEL_PID, VD_THREAD_SETTER_RESOLVER_MODULE, &tai_module);
    if(record->module_lookup_result >= 0)
    {
        record->flags |=
            VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK;
        record->module_id = (int32_t)tai_module.modid;
        record->module_nid = tai_module.module_nid;
        record->exports_start = (uint32_t)tai_module.exports_start;
        record->exports_end = (uint32_t)tai_module.exports_end;
        copy_module_name(record->module_name, tai_module.name);
        if(vd_thread_setter_resolver_name_is_expected(record->module_name))
            record->flags |=
                VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK;
    }

    if(record->module_lookup_result >= 0)
    {
        module.size = sizeof(module);
        record->module_info_result = ksceKernelGetModuleInfo(
            KERNEL_PID, tai_module.modid, &module);
        if(record->module_info_result >= 0)
        {
            record->flags |=
                VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK;
            capture_segments(record, &module);
            if(vd_thread_setter_resolver_exports_bounded(record))
                record->flags |=
                    VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED;
        }
    }

    if((record->flags &
        (VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK |
         VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK |
         VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK |
         VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED)) ==
       (VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK |
        VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK |
        VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK |
        VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED))
    {
        for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
            capture_target(record, &record->targets[i]);
    }
    vd_thread_setter_resolver_finalize(record);
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
       record.sequence == 0 || record.revision > UINT32_MAX - 2u)
        return SCE_KERNEL_START_NO_RESIDENT;

    record.revision++;
    record.state = VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED;
    if(write_next(current_slot, &record, &current_slot) < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    collect_read_only_state(&record);
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
