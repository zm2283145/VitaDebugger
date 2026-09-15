#ifndef VD_MODULE_INFO_LOOKUP_H
#define VD_MODULE_INFO_LOOKUP_H

#include <stdint.h>

/*
 * SceModulemgrForKernel is not a loadable static import for this retail
 * kernel module.  VitaShell resolves the known module-info entry point from
 * SceKernelModulemgr at runtime, using the current export first and the legacy
 * export as a compatibility fallback.
 */
#define VD_MODULE_INFO_MODULE "SceKernelModulemgr"
#define VD_MODULE_INFO_CURRENT_LIBRARY_NID UINT32_C(0xC445FA63)
#define VD_MODULE_INFO_CURRENT_FUNCTION_NID UINT32_C(0xD269F915)
#define VD_MODULE_INFO_LEGACY_LIBRARY_NID UINT32_C(0x92C9FFC2)
#define VD_MODULE_INFO_LEGACY_FUNCTION_NID UINT32_C(0xDAA90093)
#define VD_MODULE_INFO_EMPTY_ADDRESS (-1)

typedef int (*vd_module_export_lookup_fn)(int32_t pid, const char* module,
                                          uint32_t library_nid,
                                          uint32_t function_nid,
                                          uintptr_t* address);

static inline int vd_resolve_kernel_module_info_export(
    vd_module_export_lookup_fn lookup, int32_t pid, uintptr_t* address)
{
    uintptr_t candidate = 0;
    int result;

    if(!lookup || !address)
        return VD_MODULE_INFO_EMPTY_ADDRESS;

    *address = 0;
    result = lookup(pid, VD_MODULE_INFO_MODULE,
                    VD_MODULE_INFO_CURRENT_LIBRARY_NID,
                    VD_MODULE_INFO_CURRENT_FUNCTION_NID, &candidate);
    if(result >= 0 && candidate != 0)
    {
        *address = candidate;
        return result;
    }

    candidate = 0;
    result = lookup(pid, VD_MODULE_INFO_MODULE,
                    VD_MODULE_INFO_LEGACY_LIBRARY_NID,
                    VD_MODULE_INFO_LEGACY_FUNCTION_NID, &candidate);
    if(result >= 0 && candidate != 0)
    {
        *address = candidate;
        return result;
    }

    return result < 0 ? result : VD_MODULE_INFO_EMPTY_ADDRESS;
}

#endif
