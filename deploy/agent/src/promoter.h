#ifndef VITADEVDEPLOY_PROMOTER_H
#define VITADEVDEPLOY_PROMOTER_H

#include <stdint.h>

typedef void (*VdevPromoteProgressCallback)(int state,
                                            uint64_t elapsed_milliseconds,
                                            void *context);
typedef int (*VdevPromoteReadyCallback)(void *context);

enum {
    /* Negative values are local progress sentinels, not Vita installer states. */
    VDEV_PROMOTE_PROGRESS_STATE_UNAVAILABLE = -1,
    VDEV_PROMOTE_PROGRESS_RESULT_UNAVAILABLE = -2
};

typedef struct VdevPromoteReport {
    int operation_code;
    int cleanup_code;
    int last_state;
    int dispatched;
    int outcome_known;
} VdevPromoteReport;

int vdev_promote_package(const char *package_directory,
                         VdevPromoteReadyCallback ready_callback,
                         void *ready_context,
                         VdevPromoteProgressCallback progress_callback,
                         void *progress_context,
                         VdevPromoteReport *report);

#endif
