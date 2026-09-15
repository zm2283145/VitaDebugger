#include "common.h"
#include "crypto.h"
#include "direct_commit_state.h"
#include "direct_intake.h"
#include "io.h"
#include "protocol.h"
#include "ui.h"

#if VDEV_ENABLE_DIRECT_TCP

#include <openssl/sha.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef VDEV_DIRECT_TCP_PORT
#define VDEV_DIRECT_TCP_PORT 18196
#endif

#define VDEV_DIRECT_GREETING_SIZE 40u
#define VDEV_DIRECT_JOB_HEADER_SIZE 28u
#define VDEV_DIRECT_ACK_SIZE 28u
#define VDEV_DIRECT_IDLE_TIMEOUT_US UINT64_C(15000000)
#define VDEV_DIRECT_PREAUTH_TIMEOUT_US UINT64_C(30000000)
#define VDEV_DIRECT_NET_MEMORY_SIZE (1024u * 1024u)
#define VDEV_SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)

#define VDEV_DIRECT_ACK_AUTHENTICATED 1u
#define VDEV_DIRECT_ACK_COMMITTED 2u

static uint8_t direct_net_memory[VDEV_DIRECT_NET_MEMORY_SIZE]
    __attribute__((aligned(64)));

static const uint8_t greeting_magic[8] = {
    'V', 'D', 'D', 'C', 'H', 'L', '0', '1'
};
static const uint8_t job_magic[8] = {
    'V', 'D', 'D', 'J', 'O', 'B', '0', '1'
};
static const uint8_t ack_magic[8] = {
    'V', 'D', 'D', 'A', 'C', 'K', '0', '1'
};

static uint32_t load_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static uint64_t load_u64_be(const uint8_t *input)
{
    return ((uint64_t)load_u32_be(input) << 32) |
           (uint64_t)load_u32_be(input + 4u);
}

static void store_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void store_u64_be(uint8_t *output, uint64_t value)
{
    store_u32_be(output, (uint32_t)(value >> 32));
    store_u32_be(output + 4u, (uint32_t)value);
}

static int would_block(int result)
{
    if ((uint32_t)result == (uint32_t)SCE_NET_ERROR_EAGAIN) return 1;
    if (result == -1) {
        int *location = sceNetErrnoLoc();
        return location != NULL &&
               (*location == SCE_NET_EAGAIN || *location == SCE_NET_EWOULDBLOCK);
    }
    return 0;
}

static int send_exact(int socket_id, const uint8_t *data, size_t size)
{
    size_t offset = 0u;
    uint64_t deadline = sceKernelGetProcessTimeWide() +
                        VDEV_DIRECT_IDLE_TIMEOUT_US;
    while (offset < size) {
        const size_t remaining = size - offset;
        const unsigned int chunk = remaining > 65536u
            ? 65536u : (unsigned int)remaining;
        const int sent = sceNetSend(socket_id, data + offset, chunk,
                                    SCE_NET_MSG_DONTWAIT);
        if (sent > 0) {
            offset += (size_t)sent;
            deadline = sceKernelGetProcessTimeWide() +
                       VDEV_DIRECT_IDLE_TIMEOUT_US;
            continue;
        }
        if (!would_block(sent)) return VDEV_ERR_NETWORK;
        if (sceKernelGetProcessTimeWide() >= deadline) {
            return VDEV_ERR_INTERRUPTED;
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelDelayThread(1000u);
    }
    return VDEV_OK;
}

static int receive_exact_until(int socket_id, uint8_t *data, size_t size,
                               uint64_t absolute_deadline)
{
    size_t offset = 0u;
    uint64_t deadline = sceKernelGetProcessTimeWide() +
                        VDEV_DIRECT_IDLE_TIMEOUT_US;
    while (offset < size) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        const size_t remaining = size - offset;
        const unsigned int chunk = remaining > 65536u
            ? 65536u : (unsigned int)remaining;
        if (absolute_deadline != UINT64_MAX && now >= absolute_deadline) {
            return VDEV_ERR_INTERRUPTED;
        }
        const int received = sceNetRecv(socket_id, data + offset, chunk,
                                        SCE_NET_MSG_DONTWAIT);
        if (received > 0) {
            offset += (size_t)received;
            deadline = sceKernelGetProcessTimeWide() +
                       VDEV_DIRECT_IDLE_TIMEOUT_US;
            continue;
        }
        if (received == 0) return VDEV_ERR_INTERRUPTED;
        if (!would_block(received)) return VDEV_ERR_NETWORK;
        if (sceKernelGetProcessTimeWide() >= deadline) {
            return VDEV_ERR_INTERRUPTED;
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelDelayThread(1000u);
    }
    return VDEV_OK;
}

static int receive_exact(int socket_id, uint8_t *data, size_t size)
{
    return receive_exact_until(socket_id, data, size, UINT64_MAX);
}

static int send_ack(int socket_id, uint32_t phase, int code,
                    uint32_t file_count, uint64_t byte_count)
{
    uint8_t frame[VDEV_DIRECT_ACK_SIZE];
    memcpy(frame, ack_magic, sizeof(ack_magic));
    store_u32_be(frame + 8u, phase);
    store_u32_be(frame + 12u, (uint32_t)(int32_t)code);
    store_u32_be(frame + 16u, file_count);
    store_u64_be(frame + 20u, byte_count);
    return send_exact(socket_id, frame, sizeof(frame));
}

static int path_is_missing_error(int result);

static int regular_file_exists_checked(const char *path, int *exists)
{
    SceIoStat stat;
    int result;
    if (path == NULL || exists == NULL) return VDEV_ERR_INTERNAL;
    *exists = 0;
    memset(&stat, 0, sizeof(stat));
    result = sceIoGetstat(path, &stat);
    if (result >= 0) {
        if (!SCE_S_ISREG(stat.st_mode)) return VDEV_ERR_EXTRA;
        *exists = 1;
        return VDEV_OK;
    }
    return path_is_missing_error(result) ? VDEV_OK : result;
}

static int result_exists(const char *job_id, int *exists)
{
    char path[VDEV_PATH_MAX];
    const int length = snprintf(path, sizeof(path), VDEV_RESULTS "/%s.result",
                                job_id);
    if (length <= 0 || length >= (int)sizeof(path)) return VDEV_ERR_LIMIT;
    return regular_file_exists_checked(path, exists);
}

static int ensure_package_parents(const char *package_root,
                                  const char *relative_path)
{
    char directory[VDEV_PATH_MAX];
    const size_t root_length = strlen(package_root);
    const size_t relative_length = strlen(relative_path);
    size_t index;
    unsigned depth = 0u;
    int result;

    if (root_length + 1u + relative_length + 1u > sizeof(directory)) {
        return VDEV_ERR_LIMIT;
    }
    memcpy(directory, package_root, root_length);
    directory[root_length] = '/';
    memcpy(directory + root_length + 1u, relative_path, relative_length + 1u);
    for (index = root_length + 1u;
         index < root_length + 1u + relative_length; ++index) {
        if (directory[index] != '/') continue;
        if (++depth > VDEV_TREE_MAX_DEPTH) return VDEV_ERR_LIMIT;
        directory[index] = '\0';
        result = vdev_ensure_directory(directory);
        directory[index] = '/';
        if (result < 0) return result;
    }
    return VDEV_OK;
}

static void remove_if_regular(const char *path)
{
    if (vdev_is_regular_file(path)) (void)sceIoRemove(path);
}

static void remove_parent_directories(char *file_path,
                                      size_t package_root_length)
{
    char *slash = strrchr(file_path, '/');
    while (slash != NULL && (size_t)(slash - file_path) > package_root_length) {
        *slash = '\0';
        (void)sceIoRmdir(file_path);
        slash = strrchr(file_path, '/');
    }
}

static int path_is_missing_error(int result)
{
    return (uint32_t)result == VDEV_SCE_ERROR_ERRNO_ENOENT;
}

static int observe_job_directory(const char *job_directory,
                                 VdevDirectPathObservation *observation)
{
    SceIoStat stat;
    int result;
    if (job_directory == NULL || observation == NULL) {
        return VDEV_ERR_INTERNAL;
    }
    memset(&stat, 0, sizeof(stat));
    result = sceIoGetstat(job_directory, &stat);
    *observation = vdev_direct_classify_path_probe(
        result >= 0, result < 0 && path_is_missing_error(result),
        result >= 0 && SCE_S_ISDIR(stat.st_mode));
    if (*observation == VDEV_DIRECT_PATH_IO_ERROR) return result;
    if (*observation == VDEV_DIRECT_PATH_OTHER) return VDEV_ERR_EXTRA;
    return VDEV_OK;
}

static int prove_job_directory_absent(const char *job_directory)
{
    VdevDirectPathObservation observation;
    int result = vdev_sync_parent_directory(job_directory);
    if (result < 0) return result;
    result = observe_job_directory(job_directory, &observation);
    if (result < 0) return result;
    return observation == VDEV_DIRECT_PATH_MISSING
        ? VDEV_OK : VDEV_ERR_EXTRA;
}

/* Remove only paths authenticated by the incoming manifest. Unexpected data
 * makes the final directory removals fail and is never recursively deleted. */
static int reset_uncommitted_job(const char *job_directory,
                                 const VdevManifest *manifest,
                                 int prove_absence)
{
    char package_root[VDEV_PATH_MAX];
    VdevDirectPathObservation observation;
    uint32_t index;
    int result;

    result = observe_job_directory(job_directory, &observation);
    if (result < 0) return result;
    if (observation == VDEV_DIRECT_PATH_MISSING) {
        return prove_absence
            ? prove_job_directory_absent(job_directory) : VDEV_OK;
    }
    if (vdev_path_join(package_root, sizeof(package_root), job_directory,
                       "package") < 0) {
        return VDEV_ERR_LIMIT;
    }
    for (index = manifest->count; index > 0u; --index) {
        char path[VDEV_PATH_MAX];
        char part_path[VDEV_PATH_MAX];
        const VdevManifestEntry *entry = &manifest->entries[index - 1u];
        if (vdev_path_join(path, sizeof(path), package_root, entry->path) < 0 ||
            strlen(path) + sizeof(".part") > sizeof(part_path)) {
            return VDEV_ERR_LIMIT;
        }
        strcpy(part_path, path);
        strcat(part_path, ".part");
        remove_if_regular(part_path);
        remove_if_regular(path);
        remove_parent_directories(path, strlen(package_root));
    }
    if (vdev_is_directory(package_root)) (void)sceIoRmdir(package_root);
    {
        static const char *metadata[] = {
            "request.v1", "request.v1.part", "manifest.v1",
            "manifest.v1.part", "signature.bin", "signature.bin.part"
        };
        size_t metadata_index;
        for (metadata_index = 0u;
             metadata_index < sizeof(metadata) / sizeof(metadata[0]);
             ++metadata_index) {
            char path[VDEV_PATH_MAX];
            if (vdev_path_join(path, sizeof(path), job_directory,
                               metadata[metadata_index]) < 0) {
                return VDEV_ERR_LIMIT;
            }
            remove_if_regular(path);
        }
    }
    result = sceIoRmdir(job_directory);
    if (result < 0 || vdev_is_directory(job_directory) ||
        vdev_is_regular_file(job_directory)) {
        return VDEV_ERR_EXTRA;
    }
    /* A negative acknowledgement after staging ownership is a proof of
     * pre-commit failure, so make removal durable and recheck absence before
     * sending it. A stat I/O error or wrong file type never means "missing". */
    return prove_job_directory_absent(job_directory);
}

static void record_request_commit(VdevAtomicTraceStep step,
                                   VdevAtomicTraceEvent event,
                                   int code, void *context)
{
    VdevDirectTransaction *transaction = (VdevDirectTransaction *)context;
    if (transaction != NULL && step == VDEV_ATOMIC_STEP_RENAME &&
        event == VDEV_ATOMIC_TRACE_RESULT && code >= 0) {
        vdev_direct_transaction_record_request_rename(transaction);
    }
}

static int receive_file(int socket_id, const char *final_path,
                        const VdevManifestEntry *entry)
{
    uint8_t buffer[65536];
    uint8_t digest[VDEV_SHA256_LEN];
    char part_path[VDEV_PATH_MAX];
    SHA256_CTX hash;
    SceUID descriptor;
    uint64_t remaining = entry->size;
    int result = VDEV_OK;

    if (strlen(final_path) + sizeof(".part") > sizeof(part_path)) {
        return VDEV_ERR_LIMIT;
    }
    strcpy(part_path, final_path);
    strcat(part_path, ".part");
    remove_if_regular(part_path);
    descriptor = sceIoOpen(part_path,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           VDEV_CREATE_FILE_MODE);
    if (descriptor < 0) return descriptor;
    if (SHA256_Init(&hash) != 1) result = VDEV_ERR_INTERNAL;
    while (result == VDEV_OK && remaining > 0u) {
        const size_t chunk = remaining > sizeof(buffer)
            ? sizeof(buffer) : (size_t)remaining;
        size_t written = 0u;
        result = receive_exact(socket_id, buffer, chunk);
        if (result < 0) break;
        if (SHA256_Update(&hash, buffer, chunk) != 1) {
            result = VDEV_ERR_INTERNAL;
            break;
        }
        while (written < chunk) {
            const SceSSize amount = sceIoWrite(
                descriptor, buffer + written, (SceSize)(chunk - written));
            if (amount <= 0) {
                result = amount < 0 ? (int)amount : VDEV_ERR_CHANGED;
                break;
            }
            written += (size_t)amount;
        }
        remaining -= (uint64_t)chunk;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }
    if (result == VDEV_OK && SHA256_Final(digest, &hash) != 1) {
        result = VDEV_ERR_INTERNAL;
    }
    memset(&hash, 0, sizeof(hash));
    memset(buffer, 0, sizeof(buffer));
    if (result == VDEV_OK &&
        !vdev_constant_time_equal(digest, entry->sha256, sizeof(digest))) {
        result = VDEV_ERR_HASH;
    }
    memset(digest, 0, sizeof(digest));
    if (result == VDEV_OK) {
        const int sync_result = sceIoSyncByFd(descriptor, 0);
        if (sync_result < 0) result = sync_result;
    }
    {
        const int close_result = sceIoClose(descriptor);
        if (result == VDEV_OK && close_result < 0) result = close_result;
    }
    if (result < 0) {
        remove_if_regular(part_path);
        return result;
    }
    result = sceIoRename(part_path, final_path);
    if (result < 0) {
        remove_if_regular(part_path);
        return result;
    }
    result = vdev_sha256_file(final_path, entry->size, digest);
    if (result >= 0 &&
        !vdev_constant_time_equal(digest, entry->sha256, sizeof(digest))) {
        result = VDEV_ERR_HASH;
    }
    memset(digest, 0, sizeof(digest));
    if (result < 0) return result;
    return vdev_sync_parent_directory(final_path);
}

static int handle_client(int socket_id, const char *challenge_nonce)
{
    uint8_t greeting[VDEV_DIRECT_GREETING_SIZE];
    uint8_t header[VDEV_DIRECT_JOB_HEADER_SIZE];
    uint8_t signature[VDEV_SIGNATURE_LEN];
    uint8_t manifest_digest[VDEV_SHA256_LEN];
    uint8_t *request_data = NULL;
    uint8_t *manifest_data = NULL;
    uint32_t request_size;
    uint32_t manifest_size;
    uint32_t signature_size;
    uint64_t package_size;
    VdevRequest request;
    VdevManifest manifest;
    char job_directory[VDEV_PATH_MAX] = {0};
    char package_directory[VDEV_PATH_MAX] = {0};
    char request_path[VDEV_PATH_MAX] = {0};
    char manifest_path[VDEV_PATH_MAX] = {0};
    char signature_path[VDEV_PATH_MAX] = {0};
    uint64_t committed_bytes = 0u;
    uint64_t preauth_deadline;
    VdevDirectTransaction transaction;
    uint32_t index;
    int existing;
    int result;

    memset(&request, 0, sizeof(request));
    memset(&manifest, 0, sizeof(manifest));
    vdev_direct_transaction_init(&transaction);
    memcpy(greeting, greeting_magic, sizeof(greeting_magic));
    result = vdev_hex_decode(challenge_nonce, VDEV_NONCE_HEX_LEN,
                             greeting + 8u, 32u);
    if (result < 0 ||
        (result = send_exact(socket_id, greeting, sizeof(greeting))) < 0) {
        goto cleanup;
    }
    preauth_deadline = sceKernelGetProcessTimeWide() +
                       VDEV_DIRECT_PREAUTH_TIMEOUT_US;
    result = receive_exact_until(socket_id, header, sizeof(header),
                                 preauth_deadline);
    if (result < 0) goto cleanup;
    if (memcmp(header, job_magic, sizeof(job_magic)) != 0) {
        result = VDEV_ERR_FORMAT;
        goto reject_auth;
    }
    request_size = load_u32_be(header + 8u);
    manifest_size = load_u32_be(header + 12u);
    signature_size = load_u32_be(header + 16u);
    package_size = load_u64_be(header + 20u);
    if (request_size == 0u || request_size > VDEV_REQUEST_MAX ||
        manifest_size == 0u || manifest_size > VDEV_MANIFEST_MAX ||
        signature_size != VDEV_SIGNATURE_LEN ||
        package_size > VDEV_MAX_TOTAL_SIZE) {
        result = VDEV_ERR_LIMIT;
        goto reject_auth;
    }
    request_data = (uint8_t *)malloc((size_t)request_size + 1u);
    manifest_data = (uint8_t *)malloc((size_t)manifest_size + 1u);
    if (request_data == NULL || manifest_data == NULL) {
        result = VDEV_ERR_OOM;
        goto reject_auth;
    }
    if ((result = receive_exact_until(socket_id, request_data, request_size,
                                      preauth_deadline)) < 0 ||
        (result = receive_exact_until(socket_id, manifest_data, manifest_size,
                                      preauth_deadline)) < 0 ||
        (result = receive_exact_until(socket_id, signature, signature_size,
                                      preauth_deadline)) < 0) {
        goto cleanup;
    }
    request_data[request_size] = 0;
    manifest_data[manifest_size] = 0;
    result = vdev_parse_request(request_data, request_size, &request);
    if (result < 0) goto reject_auth;
    if (strcmp(request.nonce, challenge_nonce) != 0) {
        result = VDEV_ERR_SIGNATURE;
        goto reject_auth;
    }
    result = vdev_sha256_buffer(manifest_data, manifest_size,
                                manifest_digest);
    if (result < 0) goto reject_auth;
    if (!vdev_constant_time_equal(manifest_digest, request.manifest_sha256,
                                  sizeof(manifest_digest))) {
        result = VDEV_ERR_HASH;
        goto reject_auth;
    }
    result = vdev_verify_job_signature(signature, request_data, request_size,
                                       manifest_data, manifest_size);
    if (result < 0) goto reject_auth;
    /* Do not expose the dynamic manifest parser or its duplicate-path walk to
     * unauthenticated input. The signature covers the exact request and
     * manifest bytes, so it can be checked immediately after the bounded hash. */
    result = vdev_parse_manifest(manifest_data, manifest_size, &manifest);
    if (result < 0) goto reject_auth;
    if (manifest.count != request.file_count ||
        manifest.total_size != request.total_size ||
        package_size != request.total_size) {
        result = VDEV_ERR_FORMAT;
        goto reject_auth;
    }
    result = result_exists(request.job, &existing);
    if (result < 0) goto reject_auth;
    if (existing) {
        result = VDEV_ERR_REPLAY;
        goto reject_auth;
    }
    if (vdev_path_join(job_directory, sizeof(job_directory), VDEV_INBOX,
                       request.job) < 0 ||
        vdev_path_join(request_path, sizeof(request_path), job_directory,
                       "request.v1") < 0) {
        result = VDEV_ERR_LIMIT;
        goto reject_auth;
    }
    result = regular_file_exists_checked(request_path, &existing);
    if (result < 0) goto reject_auth;
    if (existing) {
        result = VDEV_ERR_REPLAY;
        goto reject_auth;
    }
    vdev_direct_transaction_take_staging(&transaction);

    /* An authenticated retry may replace only the uncommitted tree for its
     * own signed job ID. This turns a dropped connection into a clean restart
     * and never treats a byte offset as durable resume state. */
    result = reset_uncommitted_job(job_directory, &manifest, 0);
    if (result < 0 ||
        (result = vdev_ensure_directory(job_directory)) < 0 ||
        (result = vdev_path_join(package_directory, sizeof(package_directory),
                                 job_directory, "package")) < 0 ||
        (result = vdev_ensure_directory(package_directory)) < 0 ||
        (result = vdev_path_join(manifest_path, sizeof(manifest_path),
                                 job_directory, "manifest.v1")) < 0 ||
        (result = vdev_path_join(signature_path, sizeof(signature_path),
                                 job_directory, "signature.bin")) < 0 ||
        (result = vdev_write_atomic(manifest_path, manifest_data,
                                    manifest_size, 0, NULL, NULL)) < 0 ||
        (result = vdev_write_atomic(signature_path, signature,
                                    sizeof(signature), 0, NULL, NULL)) < 0) {
        goto reject_owned_auth;
    }

    result = send_ack(socket_id, VDEV_DIRECT_ACK_AUTHENTICATED,
                      VDEV_OK, 0u, 0u);
    if (result < 0) goto reject_owned_no_reply;
    vdev_ui_status(18, "Receiving signed package",
                   "Streaming authenticated files directly from the development PC");
    for (index = 0u; index < manifest.count; ++index) {
        char path[VDEV_PATH_MAX];
        result = ensure_package_parents(package_directory,
                                        manifest.entries[index].path);
        if (result < 0 ||
            vdev_path_join(path, sizeof(path), package_directory,
                           manifest.entries[index].path) < 0) {
            if (result >= 0) result = VDEV_ERR_LIMIT;
            goto reject_payload;
        }
        result = receive_file(socket_id, path, &manifest.entries[index]);
        if (result < 0) goto reject_payload;
        committed_bytes += manifest.entries[index].size;
    }

    /* request.v1 is still the sole commit point. The existing processing path
     * cannot observe this job until every authenticated file is durable. */
    result = vdev_write_atomic(request_path, request_data, request_size,
                               0, record_request_commit, &transaction);
    if (result < 0) {
        if (vdev_direct_transaction_failure(&transaction, VDEV_OK) ==
            VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT) {
            /* The namespace commit happened before a post-stat or durability
             * error. Never delete or describe this as a definite pre-commit
             * failure; the ordinary scanner must reconcile it. */
            result = VDEV_ERR_COMMIT_UNKNOWN;
            (void)send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, result,
                           manifest.count, committed_bytes);
            goto cleanup;
        }
        goto reject_payload;
    }
    /* The job and any nested package directories may all have been created in
     * this session. Syncing request.v1's immediate parent is not sufficient on
     * a filesystem where directory SyncByFd succeeds but does not cover the
     * job's link from inbox or its newly created descendants. Require one
     * checked device-wide barrier after the sole commit rename and before the
     * receiver is allowed to report known_committed. */
    result = vdev_sync_device();
    if (result < 0) {
        result = VDEV_ERR_COMMIT_UNKNOWN;
        (void)send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, result,
                       manifest.count, committed_bytes);
        goto cleanup;
    }
    result = send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, VDEV_OK,
                      manifest.count, committed_bytes);
    if (result < 0) goto cleanup;
    result = VDEV_OK;
    goto cleanup;

reject_payload:
    {
        const int cleanup_result =
            reset_uncommitted_job(job_directory, &manifest, 1);
        if (vdev_direct_transaction_failure(&transaction, cleanup_result) ==
            VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT) {
            result = VDEV_ERR_COMMIT_UNKNOWN;
        }
    }
    (void)send_ack(socket_id, VDEV_DIRECT_ACK_COMMITTED, result,
                   index, committed_bytes);
    goto cleanup;

reject_owned_auth:
    {
        const int original_result = result;
        const int cleanup_result =
            reset_uncommitted_job(job_directory, &manifest, 1);
        result = vdev_direct_transaction_failure(&transaction, cleanup_result) ==
                 VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT
            ? VDEV_ERR_COMMIT_UNKNOWN : original_result;
    }
    (void)send_ack(socket_id, VDEV_DIRECT_ACK_AUTHENTICATED,
                   result, 0u, 0u);
    goto cleanup;

reject_owned_no_reply:
    {
        const int original_result = result;
        const int cleanup_result =
            reset_uncommitted_job(job_directory, &manifest, 1);
        result = vdev_direct_transaction_failure(&transaction, cleanup_result) ==
                 VDEV_DIRECT_AMBIGUOUS_AFTER_COMMIT
            ? VDEV_ERR_COMMIT_UNKNOWN : original_result;
    }
    goto cleanup;

reject_auth:
    (void)send_ack(socket_id, VDEV_DIRECT_ACK_AUTHENTICATED, result, 0u, 0u);

cleanup:
    if (request_data != NULL) {
        memset(request_data, 0, request_size);
        free(request_data);
    }
    if (manifest_data != NULL) free(manifest_data);
    memset(signature, 0, sizeof(signature));
    memset(manifest_digest, 0, sizeof(manifest_digest));
    vdev_free_manifest(&manifest);
    return result;
}

int vdev_direct_start(VdevDirectServer *server)
{
    SceNetInitParam init;
    SceNetSockaddrIn address;
    int enabled = 1;
    int result;

    if (server == NULL) return VDEV_ERR_INTERNAL;
    memset(server, 0, sizeof(*server));
    server->listener = -1;
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0) return result;
    server->net_module_loaded = 1;
    memset(&init, 0, sizeof(init));
    init.memory = direct_net_memory;
    init.size = (int)sizeof(direct_net_memory);
    result = sceNetInit(&init);
    if (result < 0) goto failure;
    server->net_initialized = 1;
    server->listener = sceNetSocket("VitaDevDeploy intake",
                                    SCE_NET_AF_INET,
                                    SCE_NET_SOCK_STREAM,
                                    SCE_NET_IPPROTO_TCP);
    if (server->listener < 0) { result = server->listener; goto failure; }
    result = sceNetSetsockopt(server->listener, SCE_NET_SOL_SOCKET,
                              SCE_NET_SO_REUSEADDR, &enabled,
                              (unsigned int)sizeof(enabled));
    if (result < 0) goto failure;
    result = sceNetSetsockopt(server->listener, SCE_NET_SOL_SOCKET,
                              SCE_NET_SO_NBIO, &enabled,
                              (unsigned int)sizeof(enabled));
    if (result < 0) goto failure;
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons((unsigned short)VDEV_DIRECT_TCP_PORT);
    address.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    result = sceNetBind(server->listener, (const SceNetSockaddr *)&address,
                        (unsigned int)sizeof(address));
    if (result < 0) goto failure;
    result = sceNetListen(server->listener, 1);
    if (result < 0) goto failure;
    return VDEV_OK;

failure:
    vdev_direct_stop(server);
    return result < 0 ? result : VDEV_ERR_NETWORK;
}

int vdev_direct_poll(VdevDirectServer *server, const char *challenge_nonce)
{
    int client;
    int enabled = 1;
    int result;
    if (server == NULL || server->listener < 0 || challenge_nonce == NULL) {
        return VDEV_ERR_INTERNAL;
    }
    client = sceNetAccept(server->listener, NULL, NULL);
    if (client < 0) {
        return would_block(client) ? 1 : VDEV_ERR_NETWORK;
    }
    result = sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                              &enabled, (unsigned int)sizeof(enabled));
    if (result >= 0) result = handle_client(client, challenge_nonce);
    (void)sceNetShutdown(client, SCE_NET_SHUT_RDWR);
    (void)sceNetSocketClose(client);
    return result == VDEV_OK ? VDEV_OK : 2;
}

void vdev_direct_stop(VdevDirectServer *server)
{
    if (server == NULL) return;
    if (server->listener >= 0) {
        (void)sceNetSocketAbort(server->listener, 0);
        (void)sceNetSocketClose(server->listener);
        server->listener = -1;
    }
    if (server->net_initialized) {
        (void)sceNetTerm();
        server->net_initialized = 0;
    }
    if (server->net_module_loaded) {
        (void)sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        server->net_module_loaded = 0;
    }
}

#else

#include <string.h>

int vdev_direct_start(VdevDirectServer *server)
{
    if (server != NULL) {
        memset(server, 0, sizeof(*server));
        server->listener = -1;
    }
    return VDEV_ERR_GATE;
}

int vdev_direct_poll(VdevDirectServer *server, const char *challenge_nonce)
{
    (void)server;
    (void)challenge_nonce;
    return 1;
}

void vdev_direct_stop(VdevDirectServer *server)
{
    (void)server;
}

#endif
