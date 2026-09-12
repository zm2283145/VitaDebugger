#ifndef VITADEVDEPLOY_RESULT_H
#define VITADEVDEPLOY_RESULT_H

#include "common.h"

#include <psp2/types.h>

typedef struct VdevJournal {
    SceUID descriptor;
    unsigned sequence;
    char part_path[VDEV_PATH_MAX];
    char final_path[VDEV_PATH_MAX];
} VdevJournal;

int vdev_journal_start(VdevJournal *journal, const char *job_id);
int vdev_journal_event(VdevJournal *journal, const char *stage,
                       int code, const char *message);
int vdev_journal_finish(VdevJournal *journal);
void vdev_journal_abort(VdevJournal *journal);
int vdev_write_result(const char *job_id, int success, const char *stage,
                      int code, const char *title_id, const char *message);

#endif
