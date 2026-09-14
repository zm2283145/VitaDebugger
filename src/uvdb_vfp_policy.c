#include "uvdb_vfp_policy.h"

#include "vitadebug_kernel.h"

enum uvdb_vfp_snapshot_disposition uvdb_vfp_classify_snapshot_result(
    int result)
{
    if(result >= 0)
        return UVDB_VFP_SNAPSHOT_READY;
    if(result == VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE)
        return UVDB_VFP_SNAPSHOT_UNAVAILABLE;
    return UVDB_VFP_SNAPSHOT_FATAL;
}
