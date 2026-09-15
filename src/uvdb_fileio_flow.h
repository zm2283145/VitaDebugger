#pragma once

#include <stdint.h>

#include "uvdb_rsp.h"

#define UVDB_FILEIO_SIGINT 2

enum uvdb_fileio_context {
    UVDB_FILEIO_CONTEXT_UNSTOPPED = 0,
    UVDB_FILEIO_CONTEXT_REAL_STOP = 1,
};

enum uvdb_fileio_action {
    UVDB_FILEIO_ACTION_RESUME = 0,
    UVDB_FILEIO_ACTION_REPORT_INTERRUPT = 1,
    UVDB_FILEIO_ACTION_FAIL_CLOSED = 2,
};

struct uvdb_fileio_transition {
    uint32_t result;
    enum uvdb_fileio_action action;
    unsigned int stop_reply_count;
    int stop_signal;
};

/* Convert a parsed File-I/O reply into an explicit target-state transition.
 * An interrupt can yield T02 only when the caller owns a real saved context
 * and coherent all-stop. The legacy unstopped syscall path instead severs the
 * protocol; reporting a fake stop with zero registers would be unsafe. */
int uvdb_fileio_transition_decide(
    const struct uvdb_rsp_fileio_result* result,
    enum uvdb_fileio_context context,
    struct uvdb_fileio_transition* transition);
