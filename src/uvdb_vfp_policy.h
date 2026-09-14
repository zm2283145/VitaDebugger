#pragma once

enum uvdb_vfp_snapshot_disposition {
    UVDB_VFP_SNAPSHOT_FATAL = -1,
    UVDB_VFP_SNAPSHOT_READY = 0,
    UVDB_VFP_SNAPSHOT_UNAVAILABLE = 1,
};

/*
 * Classify the normalized kernel-companion result. Only the public
 * context-unavailable result may preserve ARM core registers while emitting
 * unavailable VFP slots. All other failures must reject the register read.
 */
enum uvdb_vfp_snapshot_disposition uvdb_vfp_classify_snapshot_result(
    int result);
