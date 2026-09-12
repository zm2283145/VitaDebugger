#include <stdint.h>
#include <stdio.h>

#include "vd_read_ladder_record.h"

int main(void)
{
    struct vd_read_ladder_record record = {0};
    struct vd_read_ladder_record older = {0};

    record.magic = VD_READ_LADDER_MAGIC;
    record.version = VD_READ_LADDER_VERSION;
    record.size = (uint32_t)sizeof(record);
    record.revision = 3;
    record.sequence = 1;
    record.step = VD_READ_STEP_DIDR;
    record.state = VD_READ_STATE_COMPLETE;
    record.result = VD_READ_RESULT_OK;
    record.value = UINT32_C(0x35192010);
    record.core_id = 1;
    record.flags = VD_READ_FLAG_VALUE_VALID |
                   VD_READ_FLAG_CP14 |
                   VD_READ_FLAG_GENERAL_REGS_ONLY |
                   VD_READ_FLAG_IRQ_GUARDED |
                   VD_READ_FLAG_CORE_MATCH;
    record.checksum = vd_read_ladder_checksum(&record);

    if(!vd_read_ladder_record_valid(&record))
        return 1;
    older = record;
    older.revision--;
    older.checksum = vd_read_ladder_checksum(&older);
    if(!vd_read_ladder_record_valid(&older) ||
       !vd_read_ladder_revision_newer(record.revision, older.revision))
        return 2;
    record.value ^= UINT32_C(1);
    if(vd_read_ladder_record_valid(&record))
        return 3;
    if(VD_READ_STEP_LAST - VD_READ_STEP_FIRST + 1 != 10)
        return 4;

    puts("staged read-only ladder record/ordering: PASS");
    return 0;
}
