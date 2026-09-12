#ifndef VITADEVDEPLOY_IO_H
#define VITADEVDEPLOY_IO_H

#include <stddef.h>
#include <stdint.h>

/* Stable diagnostic identifiers for the challenge atomic-write trace.  Keep
 * these numeric values in sync with host/vitadevdeploy/startup.py. */
typedef enum VdevAtomicTraceStep {
    VDEV_ATOMIC_STEP_NONE = 0,
    VDEV_ATOMIC_STEP_OPEN = 1,
    VDEV_ATOMIC_STEP_WRITE = 2,
    VDEV_ATOMIC_STEP_FILE_SYNC = 3,
    VDEV_ATOMIC_STEP_CLOSE = 4,
    VDEV_ATOMIC_STEP_RENAME = 5,
    VDEV_ATOMIC_STEP_POSTSTAT = 6,
    VDEV_ATOMIC_STEP_PARENT_DOPEN = 7,
    VDEV_ATOMIC_STEP_PARENT_SYNC = 8,
    VDEV_ATOMIC_STEP_PARENT_DCLOSE = 9,
    VDEV_ATOMIC_STEP_DEVICE_SYNC = 10
} VdevAtomicTraceStep;

typedef enum VdevAtomicTraceEvent {
    VDEV_ATOMIC_TRACE_RESET = 1,
    VDEV_ATOMIC_TRACE_ENTER = 2,
    VDEV_ATOMIC_TRACE_RESULT = 3
} VdevAtomicTraceEvent;

typedef void (*VdevAtomicTraceCallback)(VdevAtomicTraceStep step,
                                        VdevAtomicTraceEvent event,
                                        int code,
                                        void *context);

int vdev_ensure_directory(const char *path);
int vdev_is_regular_file(const char *path);
int vdev_is_directory(const char *path);
int vdev_read_file_limited(const char *path, size_t maximum,
                           uint8_t **data, size_t *size);
int vdev_write_atomic(const char *final_path, const void *data, size_t size,
                      int replace_existing,
                      VdevAtomicTraceCallback trace_callback,
                      void *trace_context);
int vdev_consume_challenge(void);
int vdev_sync_parent_directory(const char *path);
int vdev_sync_rename_parents(const char *source_path,
                             const char *destination_path);
int vdev_sha256_buffer(const void *data, size_t size, uint8_t digest[32]);
int vdev_sha256_file(const char *path, uint64_t expected_size,
                     uint8_t digest[32]);

#endif
