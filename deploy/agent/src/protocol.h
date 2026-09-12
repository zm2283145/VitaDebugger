#ifndef VITADEVDEPLOY_PROTOCOL_H
#define VITADEVDEPLOY_PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

typedef enum VdevAction {
    VDEV_ACTION_VERIFY = 0,
    VDEV_ACTION_INSTALL = 1,
    VDEV_ACTION_INSTALL_LAUNCH = 2
} VdevAction;

typedef struct VdevRequest {
    char job[VDEV_JOB_ID_HEX_LEN + 1u];
    char nonce[VDEV_NONCE_HEX_LEN + 1u];
    VdevAction action;
    char title_id[VDEV_TITLE_ID_LEN + 1u];
    uint8_t manifest_sha256[VDEV_SHA256_LEN];
    uint32_t file_count;
    uint64_t total_size;
} VdevRequest;

typedef struct VdevManifestEntry {
    uint8_t sha256[VDEV_SHA256_LEN];
    uint64_t size;
    char *path;
    int seen;
} VdevManifestEntry;

typedef struct VdevManifest {
    VdevManifestEntry *entries;
    uint32_t count;
    uint64_t total_size;
} VdevManifest;

int vdev_parse_request(const uint8_t *data, size_t size, VdevRequest *request);
int vdev_parse_manifest(const uint8_t *data, size_t size,
                        VdevManifest *manifest);
void vdev_free_manifest(VdevManifest *manifest);
int vdev_validate_job_layout(const char *job_directory);
int vdev_verify_package_tree(const char *package_directory,
                             VdevManifest *manifest);
int vdev_find_committed_job(char job_id[VDEV_JOB_ID_HEX_LEN + 1u],
                            char job_directory[VDEV_PATH_MAX]);

#endif
