#ifndef VITADEBUG_PMU_PROFILER_CLEANUP_JOURNAL_PATHS_H
#define VITADEBUG_PMU_PROFILER_CLEANUP_JOURNAL_PATHS_H

#define VD_PMU_CLEANUP_JOURNAL_PATH_CAPACITY 96u

#define VD_PMU_CLEANUP_STAGE1_RETRY_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-a.bin"
#define VD_PMU_CLEANUP_STAGE1_RETRY_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-b.bin"
#define VD_PMU_CLEANUP_STAGE1_RETRY_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-c.bin"

#define VD_PMU_CLEANUP_STAGE2_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-a.bin"
#define VD_PMU_CLEANUP_STAGE2_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-b.bin"
#define VD_PMU_CLEANUP_STAGE2_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-c.bin"

#define VD_PMU_CLEANUP_STAGE3_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-a.bin"
#define VD_PMU_CLEANUP_STAGE3_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-b.bin"
#define VD_PMU_CLEANUP_STAGE3_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-c.bin"

#define VD_PMU_CLEANUP_STAGE4_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-a.bin"
#define VD_PMU_CLEANUP_STAGE4_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-b.bin"
#define VD_PMU_CLEANUP_STAGE4_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-c.bin"

#define VD_PMU_CLEANUP_STAGE5_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-a.bin"
#define VD_PMU_CLEANUP_STAGE5_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-b.bin"
#define VD_PMU_CLEANUP_STAGE5_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-c.bin"

#define VD_PMU_CLEANUP_HISTORICAL_STAGE1_A \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-a.bin"
#define VD_PMU_CLEANUP_HISTORICAL_STAGE1_B \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-b.bin"
#define VD_PMU_CLEANUP_HISTORICAL_STAGE1_C \
    "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-c.bin"

#define VD_PMU_CLEANUP_PATH_FITS(name, path) \
    typedef char name[ \
        sizeof(path) <= VD_PMU_CLEANUP_JOURNAL_PATH_CAPACITY ? 1 : -1]

VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage1_retry_a_fits,
    VD_PMU_CLEANUP_STAGE1_RETRY_A);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage1_retry_b_fits,
    VD_PMU_CLEANUP_STAGE1_RETRY_B);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage1_retry_c_fits,
    VD_PMU_CLEANUP_STAGE1_RETRY_C);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage2_a_fits, VD_PMU_CLEANUP_STAGE2_A);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage2_b_fits, VD_PMU_CLEANUP_STAGE2_B);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage2_c_fits, VD_PMU_CLEANUP_STAGE2_C);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage3_a_fits, VD_PMU_CLEANUP_STAGE3_A);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage3_b_fits, VD_PMU_CLEANUP_STAGE3_B);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage3_c_fits, VD_PMU_CLEANUP_STAGE3_C);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage4_a_fits, VD_PMU_CLEANUP_STAGE4_A);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage4_b_fits, VD_PMU_CLEANUP_STAGE4_B);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage4_c_fits, VD_PMU_CLEANUP_STAGE4_C);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage5_a_fits, VD_PMU_CLEANUP_STAGE5_A);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage5_b_fits, VD_PMU_CLEANUP_STAGE5_B);
VD_PMU_CLEANUP_PATH_FITS(
    vd_pmu_cleanup_stage5_c_fits, VD_PMU_CLEANUP_STAGE5_C);

#undef VD_PMU_CLEANUP_PATH_FITS

#endif
