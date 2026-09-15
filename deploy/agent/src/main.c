#include "common.h"
#include "crypto.h"
#include "direct_intake.h"
#include "io.h"
#include "promoter.h"
#include "protocol.h"
#include "result.h"
#include "sfo.h"
#include "ui.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VDEV_STARTUP_TRACE "ux0:data/VitaDevDeploy.startup"
#define VDEV_STARTUP_IO_TRACE "ux0:data/VitaDevDeploy.startup_io"
#define VDEV_STARTUP_IO_MAGIC UINT32_C(0x314F4956) /* "VIO1". */
#define VDEV_STARTUP_IO_VERSION 1u

enum {
    VDEV_STARTUP_MAIN = 1,
    VDEV_STARTUP_ROOT = 2,
    VDEV_STARTUP_INBOX = 3,
    VDEV_STARTUP_RESULTS = 4,
    VDEV_STARTUP_POWER_TICK = 5,
    VDEV_STARTUP_RNG = 6,
    VDEV_STARTUP_CHALLENGE_WRITE = 7,
    VDEV_STARTUP_WAITING = 8,
    VDEV_STARTUP_JOB_COMPLETE = 9,
    VDEV_STARTUP_SAFE_EXIT = 10
};

typedef struct VdevStartupRecord {
    uint32_t magic;
    uint32_t version;
    uint32_t stage;
    int32_t code;
} VdevStartupRecord;

typedef struct VdevStartupIoHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t event_size;
} VdevStartupIoHeader;

typedef struct VdevStartupIoEventRecord {
    uint32_t sequence;
    uint32_t step;
    uint32_t event;
    int32_t code;
} VdevStartupIoEventRecord;

typedef char VdevStartupIoHeaderMustBe16Bytes[
    sizeof(VdevStartupIoHeader) == 16u ? 1 : -1];
typedef char VdevStartupIoEventMustBe16Bytes[
    sizeof(VdevStartupIoEventRecord) == 16u ? 1 : -1];

static uint32_t startup_io_sequence;

/*
 * Keep this deliberately independent of the normal result/journal machinery.
 * It is the only evidence available when startup fails before a challenge and
 * therefore before a job ID exists.  The record contains no nonce or key data.
 */
static void record_startup_stage(uint32_t stage, int code)
{
    VdevStartupRecord record;
    SceUID descriptor;

    record.magic = UINT32_C(0x31444456); /* "VDD1" in file byte order. */
    record.version = 1u;
    record.stage = stage;
    record.code = (int32_t)code;

    descriptor = sceIoOpen(VDEV_STARTUP_TRACE,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           VDEV_CREATE_FILE_MODE);
    if (descriptor < 0) return;
    sceIoWrite(descriptor, &record, (SceSize)sizeof(record));
    sceIoClose(descriptor);
}

/*
 * This trace intentionally bypasses vdev_write_atomic and all durability
 * helpers: it observes those helpers and must never recurse into them or
 * influence their return values.  Each 16-byte event is independently
 * appended after a fixed 16-byte header.  A short final event can be ignored
 * by the host parser after a sudden stop.  No nonce, path, key, or payload data
 * is recorded.
 */
static void raw_startup_io_write(SceUID descriptor,
                                 const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0u;

    while (offset < size) {
        const SceSSize written = sceIoWrite(
            descriptor, bytes + offset, (SceSize)(size - offset));
        if (written <= 0) return;
        offset += (size_t)written;
    }
}

static void record_startup_io(VdevAtomicTraceStep step,
                              VdevAtomicTraceEvent event,
                              int code,
                              void *context)
{
    SceUID descriptor;
    (void)context;

    if (event == VDEV_ATOMIC_TRACE_RESET) {
        VdevStartupIoHeader header;
        header.magic = VDEV_STARTUP_IO_MAGIC;
        header.version = VDEV_STARTUP_IO_VERSION;
        header.header_size = (uint32_t)sizeof(header);
        header.event_size = (uint32_t)sizeof(VdevStartupIoEventRecord);
        startup_io_sequence = 0u;
        descriptor = sceIoOpen(VDEV_STARTUP_IO_TRACE,
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                               VDEV_CREATE_FILE_MODE);
        if (descriptor < 0) return;
        raw_startup_io_write(descriptor, &header, sizeof(header));
        sceIoClose(descriptor);
        return;
    }

    {
        VdevStartupIoEventRecord record;
        record.sequence = ++startup_io_sequence;
        record.step = (uint32_t)step;
        record.event = (uint32_t)event;
        record.code = (int32_t)code;
        descriptor = sceIoOpen(VDEV_STARTUP_IO_TRACE,
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                               VDEV_CREATE_FILE_MODE);
        if (descriptor < 0) return;
        raw_startup_io_write(descriptor, &record, sizeof(record));
        sceIoClose(descriptor);
    }
}

static const char *message_for_code(int code)
{
    switch (code) {
        case VDEV_OK: return "ok";
        case VDEV_ERR_OOM: return "out of memory";
        case VDEV_ERR_FORMAT: return "non-canonical or malformed input";
        case VDEV_ERR_LIMIT: return "configured safety limit exceeded";
        case VDEV_ERR_PATH: return "unsafe or non-normalized path";
        case VDEV_ERR_HASH: return "SHA-256 mismatch";
        case VDEV_ERR_EXTRA: return "unexpected file or directory";
        case VDEV_ERR_MISSING: return "required file or directory missing";
        case VDEV_ERR_SIGNATURE: return "Ed25519 signature rejected";
        case VDEV_ERR_TRUST_KEY: return "trusted Ed25519 key not configured";
        case VDEV_ERR_TITLE: return "target title ID rejected";
        case VDEV_ERR_GATE: return "requested action is disabled at build time";
        case VDEV_ERR_REPLAY: return "job ID already has a committed result";
        case VDEV_ERR_CHANGED: return "staged input changed while being read";
        case VDEV_ERR_PROMOTE_RESULT: return "Vita installer returned an unexpected result";
        case VDEV_ERR_PROMOTE_UNKNOWN: return "Vita installer outcome is unknown; do not retry automatically";
        case VDEV_ERR_STALE_STAGE: return "a previous shallow installation stage still exists";
        case VDEV_ERR_REPORTING: return "installation completed but durable reporting failed; do not retry";
        case VDEV_ERR_NETWORK: return "direct TCP network operation failed";
        case VDEV_ERR_INTERRUPTED: return "direct TCP transfer was interrupted before commit";
        case VDEV_ERR_COMMIT_UNKNOWN: return "direct TCP request commit outcome is uncertain";
        default: return "Vita API or I/O failure";
    }
}

static int create_challenge(char nonce[VDEV_NONCE_HEX_LEN + 1u])
{
    uint8_t random[VDEV_NONCE_HEX_LEN / 2u];
    char challenge[128];
    int length;
    int result = sceKernelGetRandomNumber(random, sizeof(random));
    record_startup_stage(VDEV_STARTUP_RNG, result);
    if (result < 0) return result;
    vdev_hex_encode(random, sizeof(random), nonce);
    memset(random, 0, sizeof(random));
    length = snprintf(challenge, sizeof(challenge),
                      "VITADEVDEPLOY-CHALLENGE-1\nnonce=%s\n", nonce);
    if (length < 0 || length >= (int)sizeof(challenge)) return VDEV_ERR_INTERNAL;
    sceIoRemove(VDEV_CHALLENGE_USED);
    result = vdev_write_atomic(VDEV_CHALLENGE, challenge, (size_t)length, 1,
                               record_startup_io, NULL);
    record_startup_stage(VDEV_STARTUP_CHALLENGE_WRITE, result);
    return result;
}

static int waiting_exit_requested(uint32_t *previous_buttons)
{
    SceCtrlData pad;
    uint32_t pressed;
    int samples;

    if (previous_buttons == NULL) return 0;
    memset(&pad, 0, sizeof(pad));
    samples = sceCtrlPeekBufferPositive(0, &pad, 1);
    if (samples <= 0) return 0;
    pressed = (uint32_t)pad.buttons;
    if ((pressed & SCE_CTRL_CIRCLE) != 0u &&
        (*previous_buttons & SCE_CTRL_CIRCLE) == 0u) {
        *previous_buttons = pressed;
        return 1;
    }
    *previous_buttons = pressed;
    return 0;
}

static int required_package_files(const char *package_directory)
{
    static const char *required[] = {
        "eboot.bin", "sce_sys/param.sfo", "sce_sys/package/head.bin"
    };
    size_t index;
    for (index = 0; index < sizeof(required) / sizeof(required[0]); ++index) {
        char path[VDEV_PATH_MAX];
        const int result = vdev_path_join(path, sizeof(path),
                                          package_directory, required[index]);
        if (result < 0) return result;
        if (!vdev_is_regular_file(path)) return VDEV_ERR_MISSING;
    }
    return VDEV_OK;
}

#if VDEV_ENABLE_INSTALL
typedef struct VdevFinalVerifyContext {
    const char *package_directory;
    VdevManifest *manifest;
} VdevFinalVerifyContext;

static int verify_immediately_before_dispatch(void *opaque)
{
    VdevFinalVerifyContext *context = (VdevFinalVerifyContext *)opaque;
    if (context == NULL || context->package_directory == NULL ||
        context->manifest == NULL) {
        return VDEV_ERR_INTERNAL;
    }
    vdev_ui_status(84, "Final integrity check",
                   "Re-reading every signed byte immediately before install");
    return vdev_verify_package_tree(context->package_directory,
                                    context->manifest);
}

static int write_promotion_state(const char *job_id, const char *title_id,
                                 const uint8_t manifest_digest[VDEV_SHA256_LEN])
{
    char digest_hex[VDEV_SHA256_HEX_LEN + 1u];
    char state[512];
    int length;

    vdev_hex_encode(manifest_digest, VDEV_SHA256_LEN, digest_hex);
    length = snprintf(state, sizeof(state),
                      "VITADEVDEPLOY-PROMOTE-1\n"
                      "job=%s\n"
                      "title_id=%s\n"
                      "manifest_sha256=%s\n"
                      "path=%s\n"
                      "state=reserved_or_later\n",
                      job_id, title_id, digest_hex, VDEV_PROMOTE_ROOT);
    if (length < 0 || length >= (int)sizeof(state)) {
        return VDEV_ERR_INTERNAL;
    }
    return vdev_write_atomic(VDEV_PROMOTE_STATE, state, (size_t)length, 0,
                             NULL, NULL);
}

static int clear_promotion_state(void)
{
    int result;
    /* Never discard ownership metadata while the shallow package still exists. */
    if (vdev_is_directory(VDEV_PROMOTE_ROOT) ||
        vdev_is_regular_file(VDEV_PROMOTE_ROOT)) {
        return VDEV_ERR_STALE_STAGE;
    }
    result = sceIoRemove(VDEV_PROMOTE_STATE);
    if (result < 0) return result;
    if (vdev_is_regular_file(VDEV_PROMOTE_STATE) ||
        vdev_is_directory(VDEV_PROMOTE_STATE)) {
        return VDEV_ERR_CHANGED;
    }
    return vdev_sync_parent_directory(VDEV_PROMOTE_STATE);
}
#endif

static int process_job(const char *job_id, const char *job_directory,
                       const char *challenge_nonce)
{
    char path[VDEV_PATH_MAX];
    char package_directory[VDEV_PATH_MAX];
    char job_package_directory[VDEV_PATH_MAX];
    char sfo_path[VDEV_PATH_MAX];
    char sfo_title[VDEV_TITLE_ID_LEN + 1u] = {0};
    const char *stage = "layout";
    uint8_t *request_data = NULL;
    uint8_t *manifest_data = NULL;
    uint8_t *signature_data = NULL;
    size_t request_size = 0u;
    size_t manifest_size = 0u;
    size_t signature_size = 0u;
    uint8_t manifest_digest[VDEV_SHA256_LEN];
    VdevRequest request;
    VdevManifest manifest;
    VdevJournal journal;
    int journal_started = 0;
#if VDEV_ENABLE_INSTALL
    int package_relocated = 0;
    int promotion_dispatched = 0;
    int promotion_state_written = 0;
    int clear_state_after_failure = 0;
#endif
    int result;

    memset(&request, 0, sizeof(request));
    memset(&manifest, 0, sizeof(manifest));
    memset(&journal, 0, sizeof(journal));
    journal.descriptor = -1;

    vdev_ui_status(20, "Validating deployment", "Checking the committed job layout");
    result = vdev_validate_job_layout(job_directory);
    if (result < 0) goto failure;
    stage = "request";
    vdev_ui_status(28, "Reading signed request", "Checking job identity and challenge nonce");
    if ((result = vdev_path_join(path, sizeof(path), job_directory, "request.v1")) < 0 ||
        (result = vdev_read_file_limited(path, VDEV_REQUEST_MAX,
                                         &request_data, &request_size)) < 0 ||
        (result = vdev_parse_request(request_data, request_size, &request)) < 0) {
        goto failure;
    }
    if (strcmp(request.job, job_id) != 0) {
        result = VDEV_ERR_FORMAT; goto failure;
    }
    if (strcmp(request.nonce, challenge_nonce) != 0) {
        result = VDEV_ERR_SIGNATURE; goto failure;
    }
    stage = "manifest";
    vdev_ui_status(36, "Reading package manifest", "Checking the signed file inventory");
    if ((result = vdev_path_join(path, sizeof(path), job_directory, "manifest.v1")) < 0 ||
        (result = vdev_read_file_limited(path, VDEV_MANIFEST_MAX,
                                         &manifest_data, &manifest_size)) < 0 ||
        (result = vdev_sha256_buffer(manifest_data, manifest_size,
                                     manifest_digest)) < 0) {
        goto failure;
    }
    if (!vdev_constant_time_equal(manifest_digest, request.manifest_sha256,
                                  sizeof(manifest_digest))) {
        result = VDEV_ERR_HASH; goto failure;
    }
    result = vdev_parse_manifest(manifest_data, manifest_size, &manifest);
    if (result < 0) goto failure;
    if (manifest.count != request.file_count ||
        manifest.total_size != request.total_size) {
        result = VDEV_ERR_FORMAT; goto failure;
    }

    stage = "signature";
    vdev_ui_status(44, "Authenticating deployment", "Verifying the Ed25519 signature");
    if ((result = vdev_path_join(path, sizeof(path), job_directory, "signature.bin")) < 0 ||
        (result = vdev_read_file_limited(path, VDEV_SIGNATURE_LEN,
                                         &signature_data, &signature_size)) < 0) {
        goto failure;
    }
    if (signature_size != VDEV_SIGNATURE_LEN) {
        result = VDEV_ERR_SIGNATURE; goto failure;
    }
    result = vdev_verify_job_signature(signature_data, request_data, request_size,
                                       manifest_data, manifest_size);
    if (result < 0) goto failure;
    stage = "challenge";
    result = vdev_consume_challenge();
    if (result < 0) goto failure;
    stage = "journal";
    result = vdev_journal_start(&journal, job_id);
    if (result < 0) goto failure;
    journal_started = 1;
    result = vdev_journal_event(&journal, "request", VDEV_OK,
                                "signed request committed");
    if (result < 0) goto failure;
    result = vdev_journal_event(&journal, "signature", VDEV_OK,
                                "signature and manifest accepted");
    if (result < 0) { stage = "journal"; goto failure; }

    stage = "tree";
    vdev_ui_status(55, "Verifying package", "Hashing the exact extracted package tree");
    result = vdev_path_join(job_package_directory, sizeof(job_package_directory),
                            job_directory, "package");
    if (result < 0 ||
        (result = required_package_files(job_package_directory)) < 0 ||
        (result = vdev_verify_package_tree(job_package_directory, &manifest)) < 0) {
        goto failure;
    }
    memcpy(package_directory, job_package_directory,
           strlen(job_package_directory) + 1u);
    result = vdev_journal_event(&journal, "tree", VDEV_OK,
                                "exact package tree verified");
    if (result < 0) { stage = "journal"; goto failure; }

    stage = "sfo";
    vdev_ui_status(65, "Checking target identity", "Matching the signed request to param.sfo");
    if ((result = vdev_path_join(sfo_path, sizeof(sfo_path), package_directory,
                                 "sce_sys/param.sfo")) < 0 ||
        (result = vdev_read_sfo_title_id(sfo_path, sfo_title)) < 0) {
        goto failure;
    }
    if (strcmp(sfo_title, request.title_id) != 0 ||
        !vdev_is_allowed_target_title_id(sfo_title)) {
        result = VDEV_ERR_TITLE; goto failure;
    }

    if (request.action == VDEV_ACTION_VERIFY) {
        result = vdev_journal_event(&journal, "complete", VDEV_OK,
                                    "verification completed without installation");
        if (result < 0) { stage = "journal"; goto failure; }
        result = vdev_journal_finish(&journal);
        journal_started = 0;
        if (result < 0) { stage = "journal"; goto failure; }
        result = vdev_write_result(job_id, 1, "complete", VDEV_OK,
                                   request.title_id,
                                   "verification completed without installation");
        if (result >= 0) {
            vdev_ui_complete(request.title_id,
                             "Verification completed; nothing was installed");
        }
        goto cleanup;
    }

#if !VDEV_ENABLE_INSTALL
    stage = "gate";
    result = VDEV_ERR_GATE;
    goto failure;
#else
    /*
     * VitaDB moved package promotion to a shallow fixed path after discovering
     * that deeply nested staging paths can make the Vita installer fail. The
     * signed job metadata and journal remain in the inbox; only the already
     * verified package tree crosses this same-volume rename boundary.
     */
    stage = "staging";
    vdev_ui_status(74, "Preparing Vita installer",
                   "Recording ownership of the shallow package stage");
    if (vdev_is_directory(VDEV_PROMOTE_ROOT) ||
        vdev_is_regular_file(VDEV_PROMOTE_ROOT) ||
        vdev_is_directory(VDEV_PROMOTE_STATE) ||
        vdev_is_regular_file(VDEV_PROMOTE_STATE)) {
        result = VDEV_ERR_STALE_STAGE;
        goto failure;
    }
    result = write_promotion_state(job_id, request.title_id, manifest_digest);
    if (result < 0) goto failure;
    promotion_state_written = 1;

    vdev_ui_status(78, "Preparing Vita installer",
                   "Moving the verified tree to a shallow staging path");
    result = sceIoRename(job_package_directory, VDEV_PROMOTE_ROOT);
    if (result < 0) goto failure;
    package_relocated = 1;
    if (!vdev_is_directory(VDEV_PROMOTE_ROOT) ||
        vdev_is_directory(job_package_directory) ||
        vdev_is_regular_file(job_package_directory)) {
        result = VDEV_ERR_CHANGED;
        goto failure;
    }
    result = vdev_sync_rename_parents(job_package_directory,
                                      VDEV_PROMOTE_ROOT);
    if (result < 0) goto failure;
    memcpy(package_directory, VDEV_PROMOTE_ROOT, sizeof(VDEV_PROMOTE_ROOT));

    stage = "promote";
    {
        VdevPromoteReport promote_report;
        VdevFinalVerifyContext verify_context;
        int journal_warning = 0;
        int cleanup_warning = 0;

        verify_context.package_directory = package_directory;
        verify_context.manifest = &manifest;
        vdev_ui_status(82, "Starting Vita installer",
                       "Loading the system promotion service");
        result = vdev_promote_package(package_directory,
                                      verify_immediately_before_dispatch,
                                      &verify_context,
                                      vdev_ui_promotion_progress, NULL,
                                      &promote_report);
        promotion_dispatched = promote_report.dispatched;
        if (result < 0) goto failure;

        /* PromoterUtil has now returned a known successful terminal result.
         * Failures below are reporting warnings, never installation failures. */
        if (promote_report.cleanup_code < 0) {
            cleanup_warning = 1;
            result = vdev_journal_event(
                &journal, "cleanup", promote_report.cleanup_code,
                "installation succeeded; promoter cleanup returned a warning");
            if (result < 0) journal_warning = 1;
        }
        result = vdev_journal_event(&journal, "promote", VDEV_OK,
                                    "signed package promoted");
        if (result < 0) journal_warning = 1;
        if (!journal_warning) {
            result = vdev_journal_event(
                &journal, "complete", VDEV_OK,
                request.action == VDEV_ACTION_INSTALL_LAUNCH
                    ? "installation completed; host launch pending"
                    : "installation completed");
            if (result < 0) journal_warning = 1;
        }
        if (journal_started) {
            result = vdev_journal_finish(&journal);
            journal_started = 0;
            if (result < 0) journal_warning = 1;
        }

        result = vdev_write_result(
            job_id, 1, "complete", VDEV_OK, request.title_id,
            journal_warning
                ? "installation completed; crash journal is incomplete"
                : cleanup_warning
                    ? "installation completed with a promoter cleanup warning"
                    : request.action == VDEV_ACTION_INSTALL_LAUNCH
                        ? "installation completed; host must launch"
                        : "installation completed");
        if (result < 0) {
            vdev_ui_error(
                "reporting", result,
                "The package was installed, but its durable result could not be written; do not retry");
            result = VDEV_ERR_REPORTING;
            goto cleanup;
        }

        {
            const int marker_clear_result = clear_promotion_state();
            vdev_ui_complete(
                request.title_id,
                marker_clear_result < 0
                    ? "Installed; recovery marker cleanup failed and needs review"
                    : journal_warning
                        ? "Installed; the crash journal is incomplete"
                        : cleanup_warning
                            ? "Installed with a service-cleanup warning"
                            : request.action == VDEV_ACTION_INSTALL_LAUNCH
                                ? "Installed; waiting for the host to launch it"
                                : "Installation completed successfully");
        }
        result = VDEV_OK;
    }
    goto cleanup;
#endif

failure:
#if VDEV_ENABLE_INSTALL
    /* Before PromoterUtil accepts the job, return the intact tree to its signed
     * job directory. After dispatch, leave it untouched because the system may
     * have consumed or partially consumed it and the outcome may be unknown. */
    if (package_relocated && !promotion_dispatched &&
        vdev_is_directory(VDEV_PROMOTE_ROOT)) {
        const int restore_result = sceIoRename(VDEV_PROMOTE_ROOT,
                                               job_package_directory);
        if (restore_result >= 0) {
            const int restore_sync_result =
                (!vdev_is_directory(job_package_directory) ||
                 vdev_is_directory(VDEV_PROMOTE_ROOT) ||
                 vdev_is_regular_file(VDEV_PROMOTE_ROOT))
                    ? VDEV_ERR_CHANGED
                    : vdev_sync_rename_parents(VDEV_PROMOTE_ROOT,
                                               job_package_directory);
            if (restore_sync_result >= 0) {
                clear_state_after_failure = 1;
            } else if (journal_started) {
                vdev_journal_event(
                    &journal, "cleanup", restore_sync_result,
                    "restored package path but could not commit the rename");
            }
        } else if (journal_started) {
            vdev_journal_event(&journal, "cleanup", restore_result,
                               "could not restore shallow package staging directory");
        }
    } else if (promotion_state_written && !promotion_dispatched &&
               !package_relocated) {
        clear_state_after_failure = 1;
    }
#endif
    if (journal_started) {
        const int event_result = vdev_journal_event(&journal, stage, result,
                                                     message_for_code(result));
        if (event_result >= 0) {
            const int finish_result = vdev_journal_finish(&journal);
            journal_started = 0;
            if (finish_result < 0 && result >= 0) result = finish_result;
        }
    }
    {
        const int result_write = vdev_write_result(
            job_id, 0, stage, result,
            request.title_id[0] != '\0' ? request.title_id
                                        : VDEV_AGENT_TITLE_ID,
            message_for_code(result));
#if VDEV_ENABLE_INSTALL
        if (result_write >= 0 && clear_state_after_failure) {
            (void)clear_promotion_state();
        }
#else
        (void)result_write;
#endif
        if (result_write < 0) {
            vdev_ui_error(
                "reporting", result_write,
                "The operation stopped, but its durable failure result could not be written");
        } else {
            vdev_ui_error(stage, result, message_for_code(result));
        }
    }

cleanup:
    if (journal_started) vdev_journal_abort(&journal);
    if (signature_data != NULL) {
        memset(signature_data, 0, signature_size);
        free(signature_data);
    }
    if (manifest_data != NULL) free(manifest_data);
    if (request_data != NULL) free(request_data);
    vdev_free_manifest(&manifest);
    return result;
}

int main(void)
{
    char nonce[VDEV_NONCE_HEX_LEN + 1u];
    char job_id[VDEV_JOB_ID_HEX_LEN + 1u];
    char job_directory[VDEV_PATH_MAX];
    int job_processed = 0;
    int direct_enabled = 0;
    VdevDirectServer direct_server;
    uint32_t previous_buttons = 0u;
    const char *startup_stage = "startup";
    int result;

    memset(&direct_server, 0, sizeof(direct_server));
    direct_server.listener = -1;

    record_startup_stage(VDEV_STARTUP_MAIN, VDEV_OK);
    vdev_ui_init();
    vdev_ui_status(5, "Preparing workspace", "Checking VitaDevDeploy data directories");
    startup_stage = "root";
    result = vdev_ensure_directory(VDEV_ROOT);
    record_startup_stage(VDEV_STARTUP_ROOT, result);
    if (result < 0) goto done;
    startup_stage = "inbox";
    result = vdev_ensure_directory(VDEV_INBOX);
    record_startup_stage(VDEV_STARTUP_INBOX, result);
    if (result < 0) goto done;
    startup_stage = "results";
    result = vdev_ensure_directory(VDEV_RESULTS);
    record_startup_stage(VDEV_STARTUP_RESULTS, result);
    if (result < 0) goto done;

    startup_stage = "power";
    result = sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    record_startup_stage(VDEV_STARTUP_POWER_TICK, result);
    if (result < 0) goto done;
    startup_stage = "challenge";
    vdev_ui_status(10, "Creating secure session", "Publishing a fresh one-time challenge");
    result = create_challenge(nonce);
    if (result < 0) goto done;

    /* Direct TCP is an additive transport. A listener initialization failure
     * leaves the existing filesystem/FTP challenge and commit path fully
     * available. */
    result = vdev_direct_start(&direct_server);
    if (result >= 0) direct_enabled = 1;
    result = VDEV_OK;

    record_startup_stage(VDEV_STARTUP_WAITING, VDEV_OK);
    startup_stage = "waiting";
    vdev_ui_waiting();
    for (;;) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (waiting_exit_requested(&previous_buttons)) {
            result = VDEV_OK;
            record_startup_stage(VDEV_STARTUP_SAFE_EXIT, result);
            vdev_ui_status(100, "Closing safely",
                           "Releasing the display before returning to LiveArea");
            goto done;
        }
        if (direct_enabled) {
            const int direct_result = vdev_direct_poll(&direct_server, nonce);
            if (direct_result < 0) {
                vdev_direct_stop(&direct_server);
                direct_enabled = 0;
            } else if (direct_result == 2) {
                vdev_ui_waiting();
            }
        }
        result = vdev_find_committed_job(job_id, job_directory);
        if (result == VDEV_OK) break;
        if (result < 0) goto done;
        sceKernelDelayThread(250u * 1000u);
        vdev_ui_waiting_tick();
    }
    job_processed = 1;
    result = process_job(job_id, job_directory, nonce);
    record_startup_stage(VDEV_STARTUP_JOB_COMPLETE, result);

done:
    if (result < 0 && !job_processed) {
        vdev_ui_error(startup_stage, result, message_for_code(result));
    }
    vdev_direct_stop(&direct_server);
    if (result < 0) {
        sceKernelDelayThread(10u * 1000u * 1000u);
    } else {
        sceKernelDelayThread(2u * 1000u * 1000u);
    }
    memset(nonce, 0, sizeof(nonce));
    vdev_ui_finish();
    sceKernelExitProcess(result);
    return result;
}
