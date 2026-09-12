#include <stdint.h>
#include <stdio.h>

#include "vd_hw_probe_record.h"

int main(void)
{
    struct vd_hw_probe_record record = {0};
    record.magic = VD_HW_PROBE_MAGIC;
    record.version = VD_HW_PROBE_VERSION;
    record.size = (uint32_t)sizeof(record);
    record.result = VD_HW_PROBE_OK;
    record.stage = VD_HW_PROBE_STAGE_COMPLETE;
    record.flags = VD_HW_PROBE_FLAG_AUTO_UNLOAD_SAFE;
    record.raw_midr = UINT32_C(0x410fc090);
    record.raw_didr = UINT32_C(0x35192010);
    record.checksum = vd_hw_probe_record_checksum(&record);

    if(record.checksum != vd_hw_probe_record_checksum(&record))
        return 1;
    record.raw_didr ^= UINT32_C(1);
    if(record.checksum == vd_hw_probe_record_checksum(&record))
        return 2;

    puts("one-shot hardware probe record ABI/checksum: PASS");
    return 0;
}
