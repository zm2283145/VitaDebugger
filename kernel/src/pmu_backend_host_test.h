#pragma once

#include <stdint.h>

#include "pmu_backend.h"

#if !defined(VD_PMU_BACKEND_HOST_TEST)
#error "pmu_backend_host_test.h is only for the host semantic harness"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Test-only register surface used to execute the real backend state machine
 * against a software Cortex-A9 PMU model.  The production build never sees
 * this interface: pmu_backend.c selects it only when
 * VD_PMU_BACKEND_HOST_TEST is explicitly defined by the host test compiler.
 */
enum vd_pmu_backend_host_register {
    VD_PMU_BACKEND_HOST_MIDR = 0,
    VD_PMU_BACKEND_HOST_MPIDR,
    VD_PMU_BACKEND_HOST_PMCR,
    VD_PMU_BACKEND_HOST_PMCNTENSET,
    VD_PMU_BACKEND_HOST_PMCNTENCLR,
    VD_PMU_BACKEND_HOST_PMOVSR,
    VD_PMU_BACKEND_HOST_PMSWINC,
    VD_PMU_BACKEND_HOST_PMSELR,
    VD_PMU_BACKEND_HOST_PMCCNTR,
    VD_PMU_BACKEND_HOST_PMXEVTYPER,
    VD_PMU_BACKEND_HOST_PMXEVCNTR,
    VD_PMU_BACKEND_HOST_PMUSERENR,
    VD_PMU_BACKEND_HOST_PMINTENSET,
    VD_PMU_BACKEND_HOST_PMINTENCLR,
};

enum vd_pmu_backend_host_barrier {
    VD_PMU_BACKEND_HOST_ISB = 1,
    VD_PMU_BACKEND_HOST_DSB = 2,
};

struct vd_pmu_backend_host_register_ops {
    void* context;
    uint32_t (*read)(void* context,
                     enum vd_pmu_backend_host_register reg);
    void (*write)(void* context,
                  enum vd_pmu_backend_host_register reg,
                  uint32_t value);
    void (*barrier)(void* context,
                    enum vd_pmu_backend_host_barrier barrier);
};

enum vd_pmu_backend_host_dispatch_mode {
    VD_PMU_BACKEND_HOST_DISPATCH_INLINE = 0,
    VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT = 1,
    VD_PMU_BACKEND_HOST_DISPATCH_SIGNAL_FAILURE = 2,
};

/* Initialize the private engine without creating Vita kernel objects. */
int vdPmuBackendHostTestInit(
    const struct vd_pmu_backend_host_register_ops* ops);
void vdPmuBackendHostTestReset(void);

/* Advance only the host harness monotonic clock. */
void vdPmuBackendHostTestAdvanceTimeUs(uint64_t microseconds);

/* Control and resolve the deliberately injected asynchronous paths. */
void vdPmuBackendHostTestSetDispatchMode(
    enum vd_pmu_backend_host_dispatch_mode mode);
int vdPmuBackendHostTestCompleteInflight(void);

/* Narrow visibility for assertions about retained cleanup state. */
int vdPmuBackendHostTestIsInflight(void);
int vdPmuBackendHostTestIsActive(void);
int vdPmuBackendHostTestSelectorRestorePending(void);

/* Exercise the exact production wrappers, including their safety masks. */
void vdPmuBackendHostTestWritePmcr(uint32_t value);
void vdPmuBackendHostTestWriteCounterEnableSet(uint32_t value);
void vdPmuBackendHostTestWriteCounterEnableClear(uint32_t value);
void vdPmuBackendHostTestWriteInterruptEnableClear(uint32_t value);
void vdPmuBackendHostTestWriteOverflowClear(uint32_t value);
void vdPmuBackendHostTestWriteSoftwareIncrement(uint32_t value);
void vdPmuBackendHostTestWriteSelector(uint32_t value);
void vdPmuBackendHostTestWriteEventType(uint32_t value);
void vdPmuBackendHostTestWriteEventCount(uint32_t value);

#ifdef __cplusplus
}
#endif
