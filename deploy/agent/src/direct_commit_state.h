#ifndef VITADEVDEPLOY_DIRECT_COMMIT_STATE_H
#define VITADEVDEPLOY_DIRECT_COMMIT_STATE_H

/* Pure, Vita-independent outcome model. Keep this header free of SDK types so
 * the failure matrix can be compiled and exercised by a native fake-I/O test. */
typedef enum VdevDirectCommitState {
    VDEV_DIRECT_FAIL_BEFORE_COMMIT = 0,
    VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT = 1,
    VDEV_DIRECT_KNOWN_COMMITTED = 2
} VdevDirectCommitState;

typedef enum VdevDirectPathObservation {
    VDEV_DIRECT_PATH_MISSING = 0,
    VDEV_DIRECT_PATH_DIRECTORY = 1,
    VDEV_DIRECT_PATH_OTHER = 2,
    VDEV_DIRECT_PATH_IO_ERROR = 3
} VdevDirectPathObservation;

typedef struct VdevDirectTransaction {
    int staging_owned;
    int request_renamed;
} VdevDirectTransaction;

static inline VdevDirectPathObservation vdev_direct_classify_path_probe(
    int stat_succeeded, int stat_reported_missing, int is_directory)
{
    if (!stat_succeeded) {
        return stat_reported_missing
            ? VDEV_DIRECT_PATH_MISSING : VDEV_DIRECT_PATH_IO_ERROR;
    }
    return is_directory
        ? VDEV_DIRECT_PATH_DIRECTORY : VDEV_DIRECT_PATH_OTHER;
}

static inline void vdev_direct_transaction_init(VdevDirectTransaction *state)
{
    if (state != 0) {
        state->staging_owned = 0;
        state->request_renamed = 0;
    }
}

static inline void vdev_direct_transaction_take_staging(
    VdevDirectTransaction *state)
{
    if (state != 0) state->staging_owned = 1;
}

static inline void vdev_direct_transaction_record_request_rename(
    VdevDirectTransaction *state)
{
    if (state != 0) state->request_renamed = 1;
}

static inline int vdev_direct_transaction_needs_cleanup(
    const VdevDirectTransaction *state)
{
    return state != 0 && state->staging_owned && !state->request_renamed;
}

static inline VdevDirectCommitState vdev_direct_transaction_failure(
    const VdevDirectTransaction *state, int cleanup_result)
{
    return (state != 0 && state->request_renamed) || cleanup_result < 0
        ? VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT
        : VDEV_DIRECT_FAIL_BEFORE_COMMIT;
}

#endif
