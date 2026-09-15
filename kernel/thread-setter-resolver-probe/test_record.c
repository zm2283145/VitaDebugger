#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "include/vd_thread_setter_resolver_record.h"

#define TEST_MODULE_NID UINT32_C(0xF46ED7B2)
#define TEST_BASE UINT32_C(0x81000000)

static void seal(struct vd_thread_setter_resolver_record* record)
{
    record->magic = VD_THREAD_SETTER_RESOLVER_MAGIC;
    record->version = VD_THREAD_SETTER_RESOLVER_VERSION;
    record->size = (uint32_t)sizeof(*record);
    record->checksum = 0;
    record->checksum = vd_thread_setter_resolver_checksum(record);
}

static void fill_target(struct vd_thread_setter_resolver_target* target,
                        uint32_t index, uint32_t segment_index,
                        uint32_t offset, int thumb)
{
    uint32_t byte_index;
    target->lookup_result = 0;
    target->code_address = TEST_BASE + segment_index * UINT32_C(0x1000) +
                           offset;
    target->raw_address = target->code_address | (thumb ? UINT32_C(1) : 0);
    target->segment_index = (int32_t)segment_index;
    target->segment_offset = offset;
    target->code_size = VD_THREAD_SETTER_RESOLVER_CODE_BYTES;
    target->flags = VD_THREAD_SETTER_TARGET_FLAG_RESOLVED |
                    VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT |
                    VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE |
                    VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED |
                    VD_THREAD_SETTER_TARGET_FLAG_CAPTURED;
    if(thumb)
        target->flags |= VD_THREAD_SETTER_TARGET_FLAG_THUMB;
    for(byte_index = 0; byte_index < VD_THREAD_SETTER_RESOLVER_CODE_BYTES;
        ++byte_index)
        target->code[byte_index] = (uint8_t)(index * 64u + byte_index);
}

static void make_pass(struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    vd_thread_setter_resolver_init_attempt(record);
    record->revision = 3;
    record->sequence = 1;
    record->firmware_result = 0;
    /* Deliberately use the Enso_ex-spoofable 3.74 report on a documented
     * actual 3.65 baseline. This value is metadata, not a pass condition. */
    record->firmware_version = UINT32_C(0x03740000);
    record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK;
    record->module_lookup_result = 0;
    record->module_info_result = 0;
    record->module_id = 0x10023;
    record->module_nid = TEST_MODULE_NID;
    record->exports_start = TEST_BASE + UINT32_C(0x100);
    record->exports_end = TEST_BASE + UINT32_C(0x300);
    memcpy(record->module_name, VD_THREAD_SETTER_RESOLVER_MODULE,
           sizeof(VD_THREAD_SETTER_RESOLVER_MODULE));
    record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED;
    record->segment_count = 2;
    record->segments[0].base = TEST_BASE;
    record->segments[0].memsz = UINT32_C(0x1000);
    record->segments[0].filesz = UINT32_C(0x0F00);
    record->segments[0].permissions = UINT32_C(5);
    record->segments[1].base = TEST_BASE + UINT32_C(0x1000);
    record->segments[1].memsz = UINT32_C(0x1000);
    record->segments[1].filesz = UINT32_C(0x0800);
    record->segments[1].permissions = UINT32_C(5);
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
        fill_target(&record->targets[i], i, i / 2u,
                    UINT32_C(0x400) + (i % 2u) * UINT32_C(0x100),
                    (int)(i & 1u));
    vd_thread_setter_resolver_finalize(record);
    seal(record);
}

static void make_missing_export(
    struct vd_thread_setter_resolver_record* record)
{
    make_pass(record);
    struct vd_thread_setter_resolver_target* target = &record->targets[1];
    memset(target, 0, sizeof(*target));
    target->kind = VD_THREAD_SETTER_RESOLVER_CORE_SET;
    target->nid = VD_THREAD_SETTER_NID_CORE_SET;
    target->lookup_result = -1;
    target->segment_index = -1;
    vd_thread_setter_resolver_finalize(record);
    seal(record);
}

int main(void)
{
    struct vd_thread_setter_resolver_record record;
    uint32_t original_checksum;

    assert(sizeof(record) == VD_THREAD_SETTER_RESOLVER_RECORD_SIZE);

    vd_thread_setter_resolver_init_attempt(&record);
    record.revision = 1;
    record.sequence = 1;
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));
    record.state = VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED;
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_OK);
    assert(record.flags & VD_THREAD_SETTER_RESOLVER_FLAG_ALL_CAPTURED);
    assert(vd_thread_setter_resolver_record_valid(&record));

    /* Neither an Enso_ex 3.74 report nor a normal 3.65 report changes the
     * resolver result; module identity and bytes are the hardware evidence. */
    record.firmware_version = UINT32_C(0x03650000);
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));
    assert(record.result == VD_THREAD_SETTER_RESOLVER_OK);

    record.firmware_result = -1;
    record.firmware_version = 0;
    record.flags &= ~VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK;
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));
    assert(record.result == VD_THREAD_SETTER_RESOLVER_OK);

    make_pass(&record);
    original_checksum = record.checksum;
    record.targets[0].code[0] ^= UINT8_C(0x80);
    assert(!vd_thread_setter_resolver_record_valid(&record));
    record.targets[0].code[0] ^= UINT8_C(0x80);
    assert(record.checksum == original_checksum);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_missing_export(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_MISSING);
    assert(record.resolved_count == 3);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.targets[1].raw_address = UINT32_C(0x82000001);
    record.targets[1].code_address = UINT32_C(0x82000000);
    record.targets[1].segment_index = -1;
    record.targets[1].segment_offset = 0;
    record.targets[1].code_size = 0;
    memset(record.targets[1].code, 0, sizeof(record.targets[1].code));
    record.targets[1].flags = VD_THREAD_SETTER_TARGET_FLAG_RESOLVED |
                              VD_THREAD_SETTER_TARGET_FLAG_THUMB;
    vd_thread_setter_resolver_finalize(&record);
    seal(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_ERROR_ADDRESS_RANGE);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.segments[1].permissions = UINT32_C(4);
    for(uint32_t i = 2; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        record.targets[i].flags &=
            ~(VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE |
              VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED |
              VD_THREAD_SETTER_TARGET_FLAG_CAPTURED);
        record.targets[i].code_size = 0;
        memset(record.targets[i].code, 0, sizeof(record.targets[i].code));
    }
    vd_thread_setter_resolver_finalize(&record);
    seal(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_ERROR_NOT_EXECUTABLE);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.targets[3].raw_address = TEST_BASE + UINT32_C(0x1FE1);
    record.targets[3].code_address = TEST_BASE + UINT32_C(0x1FE0);
    record.targets[3].segment_index = 1;
    record.targets[3].segment_offset = UINT32_C(0x0FE0);
    record.targets[3].flags = VD_THREAD_SETTER_TARGET_FLAG_RESOLVED |
                              VD_THREAD_SETTER_TARGET_FLAG_THUMB |
                              VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT |
                              VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE;
    record.targets[3].code_size = 0;
    memset(record.targets[3].code, 0, sizeof(record.targets[3].code));
    vd_thread_setter_resolver_finalize(&record);
    seal(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_ERROR_CODE_WINDOW);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.targets[0].nid ^= UINT32_C(1);
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.firmware_version = 0;
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    make_pass(&record);
    record.targets[0].flags &=
        ~VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED;
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    assert(vd_thread_setter_resolver_revision_newer(4, 3));
    assert(!vd_thread_setter_resolver_revision_newer(3, 4));
    assert(vd_thread_setter_resolver_revision_newer(0, UINT32_MAX));
    assert(!vd_thread_setter_resolver_revision_newer(3, 3));

    assert(vd_thread_setter_resolver_select_latest(0, 0, 0, 0, 0, 0) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_EMPTY);
    assert(vd_thread_setter_resolver_select_latest(1, 1, 5, 0, 0, 0) == 0);
    assert(vd_thread_setter_resolver_select_latest(1, 1, 5, 1, 1, 6) == 1);
    assert(vd_thread_setter_resolver_select_latest(1, 1, 5, 1, 1, 5) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_CONFLICT);
    /* A valid older PASS must not hide a torn or unreadable newer sibling. */
    assert(vd_thread_setter_resolver_select_latest(1, 1, 5, 1, 0, 0) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);
    assert(vd_thread_setter_resolver_select_latest(1, 0, 0, 1, 1, 6) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);

    puts("PASS: ThreadMgr resolver fixed record and semantic validation");
    return 0;
}
