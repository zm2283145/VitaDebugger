#ifndef VITADEVDEPLOY_COMMON_H
#define VITADEVDEPLOY_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include <psp2common/kernel/iofilemgr.h>

#ifndef VDEV_AGENT_TITLE_ID
#define VDEV_AGENT_TITLE_ID "VDEVDEP01"
#endif
#ifndef VDEV_ONLY_TARGET_TITLE_ID
#define VDEV_ONLY_TARGET_TITLE_ID ""
#endif
#define VDEV_ROOT "ux0:data/VitaDevDeploy"
#define VDEV_INBOX VDEV_ROOT "/inbox"
#define VDEV_RESULTS VDEV_ROOT "/results"
#define VDEV_CHALLENGE VDEV_ROOT "/challenge.v1"
#define VDEV_CHALLENGE_USED VDEV_ROOT "/challenge.used"
#define VDEV_PROMOTE_ROOT "ux0:/data/vdd_pkg"
#define VDEV_PROMOTE_STATE VDEV_ROOT "/promote.state"
#define VDEV_STORAGE_DEVICE "ux0:"

/* Unsafe Vita applications must grant both the user and system I/O domains
 * read/write access when creating a file. Safe applications receive the
 * system bits automatically, but spelling them out keeps both variants
 * correct and gives later SyncByFd calls the access they require. */
#define VDEV_CREATE_FILE_MODE \
    (SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS | SCE_S_IWSYS)
#define VDEV_CREATE_DIRECTORY_MODE (SCE_S_IRWXU | SCE_S_IRWXS)

#define VDEV_JOB_ID_HEX_LEN 32u
#define VDEV_NONCE_HEX_LEN 64u
#define VDEV_TITLE_ID_LEN 9u
#define VDEV_SHA256_LEN 32u
#define VDEV_SHA256_HEX_LEN 64u
#define VDEV_SIGNATURE_LEN 64u

#define VDEV_PATH_MAX 768u
#define VDEV_REL_PATH_MAX 240u
#define VDEV_REQUEST_MAX 4096u
#define VDEV_MANIFEST_MAX (4u * 1024u * 1024u)
#define VDEV_MAX_FILES 8192u
#define VDEV_TREE_MAX_DEPTH 32u
#define VDEV_MAX_FILE_SIZE (UINT64_C(256) * UINT64_C(1024) * UINT64_C(1024))
#define VDEV_MAX_TOTAL_SIZE (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))

enum {
    VDEV_OK = 0,
    VDEV_ERR_OOM = -20001,
    VDEV_ERR_FORMAT = -20002,
    VDEV_ERR_LIMIT = -20003,
    VDEV_ERR_PATH = -20004,
    VDEV_ERR_HASH = -20005,
    VDEV_ERR_EXTRA = -20006,
    VDEV_ERR_MISSING = -20007,
    VDEV_ERR_SIGNATURE = -20008,
    VDEV_ERR_TRUST_KEY = -20009,
    VDEV_ERR_TITLE = -20010,
    VDEV_ERR_GATE = -20011,
    VDEV_ERR_REPLAY = -20012,
    VDEV_ERR_CHANGED = -20013,
    VDEV_ERR_INTERNAL = -20014,
    VDEV_ERR_PROMOTE_RESULT = -20015,
    VDEV_ERR_PROMOTE_UNKNOWN = -20016,
    VDEV_ERR_STALE_STAGE = -20017,
    VDEV_ERR_REPORTING = -20018
};

int vdev_is_lower_hex(const char *text, size_t length);
int vdev_is_title_id(const char *text);
int vdev_is_allowed_target_title_id(const char *text);
int vdev_hex_decode(const char *hex, size_t hex_length,
                    uint8_t *output, size_t output_length);
void vdev_hex_encode(const uint8_t *input, size_t input_length, char *output);
int vdev_constant_time_equal(const uint8_t *left, const uint8_t *right,
                             size_t length);
int vdev_path_join(char *output, size_t output_size,
                   const char *left, const char *right);

#endif
