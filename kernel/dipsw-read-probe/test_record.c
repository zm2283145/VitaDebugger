#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "include/vd_dipsw_probe_record.h"

int main(void)
{
    struct vd_dipsw_probe_record record;

    memset(&record, 0, sizeof(record));
    assert(sizeof(record) == VD_DIPSW_PROBE_RECORD_SIZE);
    record.magic = VD_DIPSW_PROBE_MAGIC;
    record.version = VD_DIPSW_PROBE_VERSION;
    record.size = sizeof(record);
    record.revision = 3;
    record.sequence = 1;
    record.state = VD_DIPSW_PROBE_STATE_COMPLETE;
    record.debug_control = VD_DIPSW_RECONFIG_MASK;
    record.system_control = VD_DIPSW_HW_DEBUG_MASK;
    record.check_203 = 1;
    record.check_228 = 1;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_OK);
    assert(record.flags == VD_DIPSW_PROBE_FLAGS_COMPLETE);
    record.checksum = vd_dipsw_probe_checksum(&record);
    assert(vd_dipsw_probe_record_valid(&record));

    record.system_control ^= VD_DIPSW_HW_DEBUG_MASK;
    assert(!vd_dipsw_probe_record_valid(&record));
    record.system_control ^= VD_DIPSW_HW_DEBUG_MASK;
    assert(vd_dipsw_probe_record_valid(&record));

    assert(vd_dipsw_probe_revision_newer(4, 3));
    assert(!vd_dipsw_probe_revision_newer(3, 4));
    assert(vd_dipsw_probe_revision_newer(0, UINT32_MAX));
    assert(!vd_dipsw_probe_revision_newer(3, 3));

    record.debug_control = 0;
    record.system_control = 0;
    record.check_203 = 0;
    record.check_228 = 0;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_OK);
    assert(record.flags == VD_DIPSW_PROBE_FLAGS_COMPLETE);

    record.check_203 = 2;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_ERROR_CHECK_203);
    assert((record.flags & VD_DIPSW_PROBE_FLAG_CHECK_203_VALID) == 0);
    assert((record.flags & VD_DIPSW_PROBE_FLAG_CHECK_203_MATCH) == 0);

    record.check_203 = 1;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_ERROR_MISMATCH_203);

    record.check_203 = 0;
    record.check_228 = 1;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_ERROR_MISMATCH_228);

    record.check_228 = -1;
    vd_dipsw_probe_validate_readback(&record);
    assert(record.result == VD_DIPSW_PROBE_ERROR_CHECK_228);

    assert(VD_DIPSW_RECONFIG_BIT / 32u == VD_DIPSW_DEBUG_INFO_INDEX);
    assert(VD_DIPSW_RECONFIG_MASK == (UINT32_C(1) << 11));
    assert(VD_DIPSW_HW_DEBUG_BIT / 32u == VD_DIPSW_SYSTEM_INFO_INDEX);
    assert(VD_DIPSW_HW_DEBUG_MASK == (UINT32_C(1) << 4));

    puts("PASS: DIP-switch probe record, journal, and bit mappings");
    return 0;
}
