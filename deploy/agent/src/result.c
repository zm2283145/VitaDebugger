#include "result.h"

#include "io.h"

#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>

static int valid_stage(const char *stage)
{
    const unsigned char *cursor = (const unsigned char *)stage;
    if (cursor == NULL || *cursor == 0) return 0;
    while (*cursor != 0) {
        if (!((*cursor >= 'a' && *cursor <= 'z') || *cursor == '_')) return 0;
        ++cursor;
    }
    return 1;
}

static int percent_encode(const char *input, char *output, size_t output_size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    const unsigned char *cursor = (const unsigned char *)input;
    while (*cursor != 0) {
        const unsigned char value = *cursor++;
        const int unreserved = (value >= 'A' && value <= 'Z') ||
                               (value >= 'a' && value <= 'z') ||
                               (value >= '0' && value <= '9') ||
                               value == '-' || value == '.' ||
                               value == '_' || value == '~';
        const size_t needed = unreserved ? 1u : 3u;
        if (used + needed + 1u > output_size) return VDEV_ERR_LIMIT;
        if (unreserved) {
            output[used++] = (char)value;
        } else {
            output[used++] = '%';
            output[used++] = digits[value >> 4];
            output[used++] = digits[value & 15u];
        }
    }
    output[used] = '\0';
    return VDEV_OK;
}

static int journal_write(VdevJournal *journal, const char *text)
{
    const size_t length = strlen(text);
    size_t offset = 0;
    while (offset < length) {
        const SceSSize written = sceIoWrite(journal->descriptor,
                                            text + offset,
                                            (SceSize)(length - offset));
        if (written < 0) return (int)written;
        if (written == 0) return VDEV_ERR_CHANGED;
        offset += (size_t)written;
    }
    return sceIoSyncByFd(journal->descriptor, 0);
}

int vdev_journal_start(VdevJournal *journal, const char *job_id)
{
    char header[128];
    int length;
    memset(journal, 0, sizeof(*journal));
    journal->descriptor = -1;
    if (snprintf(journal->final_path, sizeof(journal->final_path),
                 VDEV_RESULTS "/%s.journal", job_id) >= (int)sizeof(journal->final_path) ||
        snprintf(journal->part_path, sizeof(journal->part_path),
                 VDEV_RESULTS "/%s.journal.part", job_id) >= (int)sizeof(journal->part_path)) {
        return VDEV_ERR_LIMIT;
    }
    if (vdev_is_regular_file(journal->final_path)) return VDEV_ERR_REPLAY;
    sceIoRemove(journal->part_path);
    journal->descriptor = sceIoOpen(journal->part_path,
                                    SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                                    VDEV_CREATE_FILE_MODE);
    if (journal->descriptor < 0) return journal->descriptor;
    length = snprintf(header, sizeof(header),
                      "VITADEVDEPLOY-JOURNAL-1\njob=%s\n", job_id);
    if (length < 0 || length >= (int)sizeof(header)) {
        vdev_journal_abort(journal); return VDEV_ERR_INTERNAL;
    }
    {
        const int result = journal_write(journal, header);
        if (result < 0) vdev_journal_abort(journal);
        return result;
    }
}

int vdev_journal_event(VdevJournal *journal, const char *stage,
                       int code, const char *message)
{
    char encoded[512];
    char record[768];
    int length;
    int result;
    if (journal == NULL || journal->descriptor < 0 || !valid_stage(stage)) {
        return VDEV_ERR_INTERNAL;
    }
    result = percent_encode(message, encoded, sizeof(encoded));
    if (result < 0) return result;
    length = snprintf(record, sizeof(record),
                      "event=%u\tstage=%s\tcode=%d\tmessage=%s\n",
                      ++journal->sequence, stage, code, encoded);
    if (length < 0 || length >= (int)sizeof(record)) return VDEV_ERR_LIMIT;
    return journal_write(journal, record);
}

int vdev_journal_finish(VdevJournal *journal)
{
    int result;
    if (journal == NULL || journal->descriptor < 0) return VDEV_ERR_INTERNAL;
    result = sceIoSyncByFd(journal->descriptor, 0);
    {
        const int close_result = sceIoClose(journal->descriptor);
        journal->descriptor = -1;
        if (result >= 0 && close_result < 0) result = close_result;
    }
    if (result < 0) return result;
    result = sceIoRename(journal->part_path, journal->final_path);
    if (result >= 0 &&
        (!vdev_is_regular_file(journal->final_path) ||
         vdev_is_regular_file(journal->part_path) ||
         vdev_is_directory(journal->part_path))) {
        result = VDEV_ERR_CHANGED;
    }
    if (result >= 0) {
        result = vdev_sync_rename_parents(journal->part_path,
                                          journal->final_path);
    }
    return result < 0 ? result : VDEV_OK;
}

void vdev_journal_abort(VdevJournal *journal)
{
    if (journal == NULL) return;
    if (journal->descriptor >= 0) {
        sceIoSyncByFd(journal->descriptor, 0);
        sceIoClose(journal->descriptor);
        journal->descriptor = -1;
    }
}

int vdev_write_result(const char *job_id, int success, const char *stage,
                      int code, const char *title_id, const char *message)
{
    char final_path[VDEV_PATH_MAX];
    char encoded[512];
    char result_text[1024];
    int length;
    int result;
    if (!valid_stage(stage) || !vdev_is_title_id(title_id)) {
        return VDEV_ERR_INTERNAL;
    }
    result = percent_encode(message, encoded, sizeof(encoded));
    if (result < 0) return result;
    if (snprintf(final_path, sizeof(final_path), VDEV_RESULTS "/%s.result", job_id) >=
        (int)sizeof(final_path)) return VDEV_ERR_LIMIT;
    length = snprintf(result_text, sizeof(result_text),
                      "VITADEVDEPLOY-RESULT-1\n"
                      "job=%s\n"
                      "state=%s\n"
                      "stage=%s\n"
                      "code=%d\n"
                      "title_id=%s\n"
                      "message=%s\n",
                      job_id, success ? "success" : "failed", stage,
                      code, title_id, encoded);
    if (length < 0 || length >= (int)sizeof(result_text)) return VDEV_ERR_LIMIT;
    return vdev_write_atomic(final_path, result_text, (size_t)length, 0,
                             NULL, NULL);
}
