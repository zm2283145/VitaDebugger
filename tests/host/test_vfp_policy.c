#include "kernel/include/vitadebug_kernel.h"
#include "uvdb_vfp_policy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char* message)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    check(VD_KERNEL_ABI_VERSION == 0x0001000Du, "ABI v1.13");
    check(VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE ==
              (int)UINT32_C(0x80028031),
          "normalized result uses stable Vita error encoding");
    check(uvdb_vfp_classify_snapshot_result(0) == UVDB_VFP_SNAPSHOT_READY,
          "success is ready");
    check(uvdb_vfp_classify_snapshot_result(1) == UVDB_VFP_SNAPSHOT_READY,
          "non-negative kernel result is ready");
    check(uvdb_vfp_classify_snapshot_result(
              VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE) ==
              UVDB_VFP_SNAPSHOT_UNAVAILABLE,
          "normalized context-unavailable result permits fallback");

    const int fatal_results[] = {
        -1,
        -2,
        -3,
        -4,
        -5,
        -6,
        VD_KERNEL_ERROR_VFP_GUARD,
        VD_KERNEL_ERROR_VFP_DISABLED,
        VD_KERNEL_ERROR_HW_DISABLED,
        VD_KERNEL_ERROR_HW_BUSY,
        VD_KERNEL_ERROR_HW_OWNER,
        VD_KERNEL_ERROR_HW_STOP_REQUIRED,
        VD_KERNEL_ERROR_HW_INVALID,
        VD_KERNEL_ERROR_HW_CORE,
        VD_KERNEL_ERROR_HW_RESTORE,
        VD_KERNEL_ERROR_HW_RANGE,
        (int)UINT32_C(0x80020008),
    };
    for(unsigned int i = 0;
        i < sizeof(fatal_results) / sizeof(fatal_results[0]); ++i)
        check(uvdb_vfp_classify_snapshot_result(fatal_results[i]) ==
                  UVDB_VFP_SNAPSHOT_FATAL,
              "all non-normalized errors fail closed");

    puts("PASS: VFP snapshot result policy");
    return 0;
}
