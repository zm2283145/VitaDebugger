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

static void fill_module(struct vd_thread_setter_resolver_record* record)
{
    record->module_lookup_result = 0;
    record->module_id = 0x10023;
    record->module_nid = TEST_MODULE_NID;
    record->exports_start = TEST_BASE + UINT32_C(0x100);
    record->exports_end = TEST_BASE + UINT32_C(0x300);
    memcpy(record->module_name, VD_THREAD_SETTER_RESOLVER_MODULE,
           sizeof(VD_THREAD_SETTER_RESOLVER_MODULE));
}

static void fill_target(struct vd_thread_setter_resolver_target* target,
                        uint32_t index)
{
    const uint32_t address = TEST_BASE + UINT32_C(0x400) +
                             index * UINT32_C(0x100);
    target->lookup_result = 0;
    target->raw_address = address | (index & 1u);
    target->code_address = address;
    target->flags = VD_THREAD_SETTER_TARGET_FLAG_RESOLVED;
    if((index & 1u) != 0)
        target->flags |= VD_THREAD_SETTER_TARGET_FLAG_THUMB;
}

static void make_progress(
    struct vd_thread_setter_resolver_record* record,
    uint32_t completed_targets)
{
    uint32_t i;
    vd_thread_setter_resolver_init_attempt(record);
    record->revision = 3u + completed_targets;
    record->sequence = 1;
    record->firmware_result = 0;
    record->firmware_version = UINT32_C(0x03740000);
    fill_module(record);
    record->state = completed_targets == 0 ?
        VD_THREAD_SETTER_RESOLVER_STATE_MODULE_RECORDED :
        VD_THREAD_SETTER_RESOLVER_STATE_TARGET_RECORDED;
    record->completed_target_count = completed_targets;
    for(i = 0; i < completed_targets; ++i)
        fill_target(&record->targets[i], i);
    record->resolved_count = completed_targets;
    seal(record);
}

static void make_complete(
    struct vd_thread_setter_resolver_record* record)
{
    make_progress(record, VD_THREAD_SETTER_RESOLVER_TARGET_COUNT);
    vd_thread_setter_resolver_finalize(record);
    record->revision++;
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

    record.state = VD_THREAD_SETTER_RESOLVER_STATE_FIRMWARE_RECORDED;
    record.firmware_result = 0;
    record.firmware_version = UINT32_C(0x03740000);
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_progress(&record, 0);
    assert(vd_thread_setter_resolver_record_valid(&record));
    for(uint32_t count = 1; count <= VD_THREAD_SETTER_RESOLVER_TARGET_COUNT;
        ++count)
    {
        make_progress(&record, count);
        assert(vd_thread_setter_resolver_record_valid(&record));
    }

    make_complete(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_OK);
    assert(record.flags & VD_THREAD_SETTER_RESOLVER_FLAG_ALL_RESOLVED);
    assert(record.executable_count == 0);
    assert(record.captured_count == 0);
    assert(vd_thread_setter_resolver_record_valid(&record));

    record.firmware_version = UINT32_C(0x03650000);
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));
    assert(record.result == VD_THREAD_SETTER_RESOLVER_OK);

    record.firmware_result = -1;
    record.firmware_version = 0;
    record.flags &= ~VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK;
    seal(&record);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    original_checksum = record.checksum;
    record.targets[0].raw_address ^= UINT32_C(2);
    assert(!vd_thread_setter_resolver_record_valid(&record));
    record.targets[0].raw_address ^= UINT32_C(2);
    assert(record.checksum == original_checksum);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    record.targets[1].lookup_result = -1;
    record.targets[1].flags = 0;
    record.targets[1].raw_address = 0;
    record.targets[1].code_address = 0;
    vd_thread_setter_resolver_finalize(&record);
    seal(&record);
    assert(record.result == VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_MISSING);
    assert(record.resolved_count == 3);
    assert(vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    record.targets[0].nid ^= UINT32_C(1);
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    record.targets[0].flags |=
        VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE;
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    record.targets[0].code_size = 1;
    record.targets[0].code[0] = UINT8_C(0xAA);
    seal(&record);
    assert(!vd_thread_setter_resolver_record_valid(&record));

    make_complete(&record);
    record.completed_target_count--;
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
    assert(vd_thread_setter_resolver_select_latest(1, 1, 5, 1, 0, 0) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);
    assert(vd_thread_setter_resolver_select_latest(1, 0, 0, 1, 1, 6) ==
           VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT);

    puts("PASS: checkpointed non-dereferencing ThreadMgr prerequisite record");
    return 0;
}
