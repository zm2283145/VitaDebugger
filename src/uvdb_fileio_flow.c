#include "uvdb_fileio_flow.h"

int uvdb_fileio_transition_decide(
    const struct uvdb_rsp_fileio_result* result,
    enum uvdb_fileio_context context,
    struct uvdb_fileio_transition* transition)
{
    if(!result || !transition ||
       (context != UVDB_FILEIO_CONTEXT_UNSTOPPED &&
        context != UVDB_FILEIO_CONTEXT_REAL_STOP) ||
       (result->interrupted != 0 && result->interrupted != 1))
        return -1;

    struct uvdb_fileio_transition candidate = {
        .result = result->result,
        .action = UVDB_FILEIO_ACTION_RESUME,
    };
    if(result->interrupted)
    {
        if(context == UVDB_FILEIO_CONTEXT_REAL_STOP)
        {
            candidate.action = UVDB_FILEIO_ACTION_REPORT_INTERRUPT;
            candidate.stop_reply_count = 1u;
            candidate.stop_signal = UVDB_FILEIO_SIGINT;
        }
        else
            candidate.action = UVDB_FILEIO_ACTION_FAIL_CLOSED;
    }
    *transition = candidate;
    return 0;
}
