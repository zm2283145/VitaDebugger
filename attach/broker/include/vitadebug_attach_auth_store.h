#pragma once

#include "vitadebug_attach_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_STORE_SCHEMA 1u
#define VD_ATTACH_AUTH_STORE_MAX_PEERS 16u
#define VD_ATTACH_AUTH_STORE_MAX_BYTES 2048u
#define VD_ATTACH_AUTH_STORE_CONTEXT_BYTES 2048u

/*
 * This path is intentionally fixed.  A Vita integration must arrange for the
 * directory and both files to be private to the broker's trusted principal.
 * VitaSDK's public sceIo API cannot establish that confidentiality property.
 */
#define VD_ATTACH_AUTH_STORE_VITA_PATH \
    "ur0:data/VitaDebugger/private/attach-auth.store"
#define VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH \
    "ur0:data/VitaDebugger/private/attach-auth.store.new"

enum {
    VD_ATTACH_AUTH_STORE_OK = 0,
    VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT = -20,
    VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE = -21,
    VD_ATTACH_AUTH_STORE_ERROR_IO = -22,
    VD_ATTACH_AUTH_STORE_ERROR_MALFORMED = -23,
    VD_ATTACH_AUTH_STORE_ERROR_STALE = -24,
    VD_ATTACH_AUTH_STORE_ERROR_LIMIT = -25,
    VD_ATTACH_AUTH_STORE_ERROR_CONFLICT = -26,
    VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED = -27,
};

enum {
    VD_ATTACH_AUTH_STORE_IO_OK = 0,
    VD_ATTACH_AUTH_STORE_IO_NOT_FOUND = 1,
};

/*
 * Successful opens MUST refer to a regular file reached without following
 * links.  Implementations must also reject multiply-linked files where their
 * platform supports links.  create_new must be exclusive.  replace_atomic
 * must atomically replace the destination, and sync_parent must make the
 * directory entry durable.  No callback may retain a supplied data pointer.
 */
typedef struct VdAttachAuthStoreFileOps {
    void *context;
    int (*open_read_regular_no_follow)(void *context,
                                       const char *path,
                                       void **handle);
    int (*create_new_regular_no_follow)(void *context,
                                        const char *path,
                                        void **handle);
    int (*read)(void *context, void *handle, uint8_t *data,
                size_t capacity, size_t *read_size);
    int (*write)(void *context, void *handle, const uint8_t *data,
                 size_t size, size_t *write_size);
    int (*sync)(void *context, void *handle);
    int (*close)(void *context, void *handle);
    int (*replace_atomic)(void *context, const char *from,
                          const char *to);
    int (*remove_regular_no_follow)(void *context, const char *path);
    int (*sync_parent)(void *context, const char *path);
} VdAttachAuthStoreFileOps;

/*
 * The floor must live outside the regular store and resist rollback by an
 * attacker able to restore an older store file.  advance_floor must durably
 * set the floor to at least revision before returning success.
 */
typedef struct VdAttachAuthStoreRollbackOps {
    void *context;
    int (*load_floor)(void *context, uint64_t *revision);
    int (*advance_floor)(void *context, uint64_t revision);
} VdAttachAuthStoreRollbackOps;

typedef struct VdAttachAuthStoreMetadata {
    uint32_t schema;
    uint64_t revision;
    uint64_t service_generation;
    size_t peer_count;
    int provisioned;
} VdAttachAuthStoreMetadata;

typedef union VdAttachAuthStore {
    max_align_t alignment;
    uint8_t opaque[VD_ATTACH_AUTH_STORE_CONTEXT_BYTES];
} VdAttachAuthStore;

int vd_attach_auth_store_init(
    VdAttachAuthStore *store,
    const VdAttachAuthStoreFileOps *file_ops,
    const VdAttachAuthStoreRollbackOps *rollback_ops);

int vd_attach_auth_store_recover(VdAttachAuthStore *store);

int vd_attach_auth_store_get_metadata(
    const VdAttachAuthStore *store,
    VdAttachAuthStoreMetadata *metadata);

/*
 * Atomically advances both the persistent revision and service generation.
 * Call this before opening a listener port so every service lifetime uses a
 * distinct, rollback-protected, nonzero generation.
 */
int vd_attach_auth_store_reserve_service_generation(
    VdAttachAuthStore *store,
    uint64_t *service_generation);

int vd_attach_auth_store_load_local_public(
    const VdAttachAuthStore *store,
    VdAttachAuthPublicKey *key);

int vd_attach_auth_store_lookup_peer(
    const VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation,
    VdAttachAuthPublicKey *key);

int vd_attach_auth_store_sign_local(
    VdAttachAuthStore *store,
    const uint8_t *message,
    size_t message_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES]);

int vd_attach_auth_store_provision(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES]);

int vd_attach_auth_store_rotate(
    VdAttachAuthStore *store,
    uint64_t old_key_id,
    uint64_t old_generation,
    uint64_t new_key_id,
    uint64_t new_generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES]);

/* Peer provisioning accepts public material only. */
int vd_attach_auth_store_allow_peer(
    VdAttachAuthStore *store,
    const VdAttachAuthPublicKey *peer);

int vd_attach_auth_store_revoke_peer(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation);

/*
 * Uninstall durably commits a key-free tombstone before returning.  Its
 * revision remains protected by the rollback floor, so restoring an older
 * file containing keys is rejected.
 */
int vd_attach_auth_store_uninstall(VdAttachAuthStore *store);

int vd_attach_auth_store_bind(VdAttachAuthStore *store,
                              VdAttachAuthKeyStorage *storage);

/*
 * Public sceIo calls provide regular file I/O, sync, and rename only.  The
 * caller-supplied proof is therefore mandatory and must attest private-path,
 * no-link, and atomic-rename properties on every use.  The externally backed
 * floor callbacks are also mandatory.  The assurance object must outlive the
 * store.
 */
typedef struct VdAttachAuthStoreVitaAssurance {
    void *context;
    int (*prove_private_storage)(void *context,
                                 const char *store_path,
                                 const char *temporary_path);
    int (*load_floor)(void *context, uint64_t *revision);
    int (*advance_floor)(void *context, uint64_t revision);
} VdAttachAuthStoreVitaAssurance;

int vd_attach_auth_store_vita_init(
    VdAttachAuthStore *store,
    VdAttachAuthStoreVitaAssurance *assurance);

#ifdef __cplusplus
}
#endif
