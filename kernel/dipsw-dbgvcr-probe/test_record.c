#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "include/vd_dipsw_dbgvcr_record.h"

static void make_valid_baseline(struct vd_dipsw_dbgvcr_record* record)
{
    memset(record, 0, sizeof(*record));
    record->before_cp_build_version = 0x12340001u;
    record->before_debug = 0x10203040u;
    record->before_system = 0x20000000u;
    record->confirm_cp_build_version = record->before_cp_build_version;
    record->confirm_debug = record->before_debug;
    record->confirm_system = record->before_system;
    record->before_check_203 = 0;
    record->before_check_228 = 0;
    record->confirm_check_203 = 0;
    record->confirm_check_228 = 0;
    vd_dipsw_dbgvcr_validate_before(record);
    vd_dipsw_dbgvcr_validate_confirm(record);
}

static void make_valid_completion(struct vd_dipsw_dbgvcr_record* record)
{
    make_valid_baseline(record);
    record->after_set_system =
        record->before_system | VD_DIPSW_HW_DEBUG_MASK;
    record->after_set_check_228 = 1;
    vd_dipsw_dbgvcr_validate_after_set(record);
    record->flags |= VD_DIPSW_DBGVCR_FLAG_IRQ_GUARDED |
                     VD_DIPSW_DBGVCR_FLAG_CORE_MATCH |
                     VD_DIPSW_DBGVCR_FLAG_CORE_ALLOWED |
                     VD_DIPSW_DBGVCR_FLAG_READ_RETURNED;
    record->dbgvcr_value = 0xA5A55A5Au;
    record->after_restore_cp_build_version =
        record->before_cp_build_version;
    record->after_restore_debug = record->before_debug;
    record->after_restore_system = record->before_system;
    record->after_restore_check_203 = record->before_check_203;
    record->after_restore_check_228 = record->before_check_228;
    vd_dipsw_dbgvcr_validate_after_restore(record);
}

int main(void)
{
    struct vd_dipsw_dbgvcr_record record;

    assert(sizeof(record) == VD_DIPSW_DBGVCR_RECORD_SIZE);
    assert(offsetof(struct vd_dipsw_dbgvcr_record, checksum) == 12u);
    assert(offsetof(struct vd_dipsw_dbgvcr_record, restore_result) == 32u);
    assert(offsetof(struct vd_dipsw_dbgvcr_record, dbgvcr_value) == 88u);
    assert(offsetof(struct vd_dipsw_dbgvcr_record, core_before) == 112u);
    assert(offsetof(struct vd_dipsw_dbgvcr_record, journal_error) == 136u);
    make_valid_completion(&record);
    assert(record.result == VD_DIPSW_DBGVCR_OK);
    assert(record.restore_result == VD_DIPSW_DBGVCR_OK);
    assert(record.flags == VD_DIPSW_DBGVCR_FLAGS_COMPLETE);

    record.magic = VD_DIPSW_DBGVCR_MAGIC;
    record.version = VD_DIPSW_DBGVCR_VERSION;
    record.size = sizeof(record);
    record.revision = 5;
    record.sequence = 1;
    record.state = VD_DIPSW_DBGVCR_STATE_COMPLETE;
    record.core_before = 1;
    record.core_read = 1;
    record.hazard_armed = 1;
    record.set_call_count = 1;
    record.dbgvcr_read_count = 1;
    record.clear_call_count = 1;
    record.checksum = vd_dipsw_dbgvcr_checksum(&record);
    assert(vd_dipsw_dbgvcr_record_valid(&record));
    record.dbgvcr_value ^= 1u;
    assert(!vd_dipsw_dbgvcr_record_valid(&record));

    make_valid_baseline(&record);
    record.confirm_system ^= 1u;
    record.result = VD_DIPSW_DBGVCR_OK;
    record.flags &= ~VD_DIPSW_DBGVCR_FLAG_BASELINE_STABLE;
    vd_dipsw_dbgvcr_validate_confirm(&record);
    assert(record.result == VD_DIPSW_DBGVCR_ERROR_BASELINE_UNSTABLE);

    make_valid_completion(&record);
    record.after_set_system ^= 1u;
    record.result = VD_DIPSW_DBGVCR_OK;
    record.flags &= ~VD_DIPSW_DBGVCR_FLAG_SET_SYSTEM_EXACT;
    vd_dipsw_dbgvcr_validate_after_set(&record);
    assert(record.result == VD_DIPSW_DBGVCR_ERROR_SET_SYSTEM_DELTA);

    make_valid_completion(&record);
    record.after_restore_system |= VD_DIPSW_HW_DEBUG_MASK;
    record.after_restore_check_228 = 1;
    record.flags &= ~(VD_DIPSW_DBGVCR_FLAG_RESTORE_228_EXACT |
                      VD_DIPSW_DBGVCR_FLAG_RESTORE_SYSTEM_EXACT);
    vd_dipsw_dbgvcr_validate_after_restore(&record);
    assert(record.result == VD_DIPSW_DBGVCR_OK);
    assert(record.restore_result ==
           VD_DIPSW_DBGVCR_ERROR_RESTORE_CHECK_228);

    assert(vd_dipsw_dbgvcr_revision_newer(6, 5));
    assert(vd_dipsw_dbgvcr_revision_newer(0, UINT32_MAX));
    assert(!vd_dipsw_dbgvcr_revision_newer(5, 5));
    assert(VD_DIPSW_DBGVCR_FLAGS_COMPLETE == UINT32_C(0x0003FFFF));

    puts("PASS: DIP 228 + one-read DBGVCR record and invariants");
    return 0;
}
