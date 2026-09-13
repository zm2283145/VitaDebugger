#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "include/vd_dipsw_set_probe_record.h"

static void make_valid_baseline(struct vd_dipsw_set_probe_record* record)
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
    vd_dipsw_set_probe_validate_before(record);
    vd_dipsw_set_probe_validate_confirm(record);
}

static void make_valid_transaction(struct vd_dipsw_set_probe_record* record)
{
    make_valid_baseline(record);
    record->after_set_system =
        record->before_system | VD_DIPSW_HW_DEBUG_MASK;
    record->after_set_check_228 = 1;
    record->after_restore_cp_build_version =
        record->before_cp_build_version;
    record->after_restore_debug = record->before_debug;
    record->after_restore_system = record->before_system;
    record->after_restore_check_203 = 0;
    record->after_restore_check_228 = 0;
    vd_dipsw_set_probe_validate_after_set(record);
    vd_dipsw_set_probe_validate_after_restore(record);
}

int main(void)
{
    struct vd_dipsw_set_probe_record record;

    assert(sizeof(record) == VD_DIPSW_SET_PROBE_RECORD_SIZE);
    make_valid_transaction(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_OK);
    assert(record.restore_result == VD_DIPSW_SET_PROBE_OK);
    assert(record.flags == VD_DIPSW_SET_FLAGS_COMPLETE);

    record.magic = VD_DIPSW_SET_PROBE_MAGIC;
    record.version = VD_DIPSW_SET_PROBE_VERSION;
    record.size = sizeof(record);
    record.revision = 5;
    record.sequence = 1;
    record.state = VD_DIPSW_SET_PROBE_STATE_COMPLETE;
    record.hazard_armed = 1;
    record.set_call_count = 1;
    record.clear_call_count = 1;
    record.checksum = vd_dipsw_set_probe_checksum(&record);
    assert(vd_dipsw_set_probe_record_valid(&record));
    record.after_set_system ^= 1u;
    assert(!vd_dipsw_set_probe_record_valid(&record));

    make_valid_baseline(&record);
    record.confirm_system ^= 1u;
    record.result = VD_DIPSW_SET_PROBE_OK;
    record.flags &= ~VD_DIPSW_SET_FLAG_BASELINE_STABLE;
    vd_dipsw_set_probe_validate_confirm(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_ERROR_BASELINE_UNSTABLE);
    assert((record.flags & VD_DIPSW_SET_FLAG_BASELINE_STABLE) == 0);

    make_valid_baseline(&record);
    record.before_check_203 = 1;
    record.confirm_check_203 = 1;
    record.before_debug |= VD_DIPSW_RECONFIG_MASK;
    record.confirm_debug = record.before_debug;
    vd_dipsw_set_probe_validate_before(&record);
    vd_dipsw_set_probe_validate_confirm(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_ERROR_RECONFIG_DISABLED);

    make_valid_baseline(&record);
    record.before_check_228 = 1;
    record.confirm_check_228 = 1;
    record.before_system |= VD_DIPSW_HW_DEBUG_MASK;
    record.confirm_system = record.before_system;
    vd_dipsw_set_probe_validate_before(&record);
    vd_dipsw_set_probe_validate_confirm(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_ERROR_228_ALREADY_SET);

    make_valid_transaction(&record);
    record.after_set_system ^= 1u;
    record.result = VD_DIPSW_SET_PROBE_OK;
    record.flags &= ~VD_DIPSW_SET_FLAG_SET_SYSTEM_EXACT;
    vd_dipsw_set_probe_validate_after_set(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_ERROR_SET_SYSTEM_DELTA);

    make_valid_transaction(&record);
    record.after_restore_system |= VD_DIPSW_HW_DEBUG_MASK;
    record.after_restore_check_228 = 1;
    record.flags &= ~(VD_DIPSW_SET_FLAG_RESTORE_228_EXACT |
                      VD_DIPSW_SET_FLAG_RESTORE_SYSTEM_EXACT);
    vd_dipsw_set_probe_validate_after_restore(&record);
    assert(record.result == VD_DIPSW_SET_PROBE_OK);
    assert(record.restore_result ==
           VD_DIPSW_SET_PROBE_ERROR_RESTORE_CHECK_228);

    make_valid_transaction(&record);
    record.after_restore_cp_build_version ^= 1u;
    record.flags &= ~VD_DIPSW_SET_FLAG_RESTORE_CP_EXACT;
    vd_dipsw_set_probe_validate_after_restore(&record);
    assert(record.restore_result ==
           VD_DIPSW_SET_PROBE_ERROR_RESTORE_CP_CHANGED);

    assert(vd_dipsw_set_probe_revision_newer(6, 5));
    assert(vd_dipsw_set_probe_revision_newer(0, UINT32_MAX));
    assert(!vd_dipsw_set_probe_revision_newer(5, 5));
    assert(VD_DIPSW_SET_FLAGS_COMPLETE == UINT32_C(0x00003FFF));

    puts("PASS: DIP 228 set/read/restore record and invariants");
    return 0;
}
