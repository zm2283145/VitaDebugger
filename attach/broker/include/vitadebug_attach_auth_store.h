#pragma once

#include "vitadebug_attach_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_STORE_SCHEMA 2u
#define VD_ATTACH_AUTH_STORE_MAX_PEERS 16u
#define VD_ATTACH_AUTH_STORE_MAX_BYTES 2048u
#define VD_ATTACH_AUTH_STORE_CONTEXT_BYTES 2048u

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
    VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY = -28,
    VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY = -29,
};

enum {
    VD_ATTACH_AUTH_STORE_BACKEND_OK = 0,
    VD_ATTACH_AUTH_STORE_BACKEND_NOT_FOUND = 1,
    /* Commit outcome is unknown; preserve staged data for recovery. */
    VD_ATTACH_AUTH_STORE_BACKEND_INTERRUPTED = 2,
};

enum {
    VD_ATTACH_AUTH_PERSISTENCE_HANDLE_BOUND = 1u << 0,
    VD_ATTACH_AUTH_PERSISTENCE_DURABLE_COMMIT = 1u << 1,
    VD_ATTACH_AUTH_PERSISTENCE_RECOVERABLE_STAGE = 1u << 2,
    VD_ATTACH_AUTH_PERSISTENCE_PATH_PROOF_ONLY = 1u << 31,
};

/*
 * All operations address backend-owned current or staged objects; the core
 * never receives a path.  commit_staged and discard_staged must act on the
 * exact handle supplied.  A successful commit atomically and durably promotes
 * that synced staged object to current without consuming the handle.  A file
 * backend must return only no-follow, already-open regular-file handles.
 * close always consumes its handle, even when it reports a diagnostic error.
 * A commit error means no promotion; an ambiguous result must be reported as
 * BACKEND_INTERRUPTED so recovery, rather than rollback, decides the outcome.
 * Capability bits and trust-domain tokens support structural fail-closed
 * checks but are not proof of trustworthiness; each backend still requires
 * independent review.
 */
typedef struct VdAttachAuthStorePersistenceOps {
    void *context;
    const void *trust_domain;
    uint32_t capabilities;
    int (*open_current)(void *context, void **handle);
    int (*open_staged)(void *context, void **handle);
    int (*create_staged)(void *context, void **handle);
    int (*read)(void *context, void *handle, uint8_t *data,
                size_t capacity, size_t *read_size);
    int (*write)(void *context, void *handle, const uint8_t *data,
                 size_t size, size_t *write_size);
    int (*sync)(void *context, void *handle);
    int (*close)(void *context, void *handle);
    int (*commit_staged)(void *context, void *staged_handle);
    int (*discard_staged)(void *context, void *staged_handle);
} VdAttachAuthStorePersistenceOps;

enum {
    VD_ATTACH_AUTH_MONOTONIC_INDEPENDENT_TRUST_DOMAIN = 1u << 0,
    VD_ATTACH_AUTH_MONOTONIC_DURABLE = 1u << 1,
    VD_ATTACH_AUTH_MONOTONIC_METADATA_NAMESPACE = 1u << 31,
};

/*
 * trust_domain must identify a security domain distinct from persistence.
 * advance_floor durably raises the floor before returning success.
 *
 * This revision-only interface cannot bind an advanced floor to the exact
 * staged metadata object. It is a host-tested model, not a sufficient
 * production transaction boundary. A production redesign must carry a digest
 * or unforgeable staged-object token, or use one trusted atomic service.
 */
typedef struct VdAttachAuthStoreMonotonicOps {
    void *context;
    const void *trust_domain;
    uint32_t capabilities;
    int (*load_floor)(void *context, uint64_t *revision);
    int (*advance_floor)(void *context, uint64_t revision);
} VdAttachAuthStoreMonotonicOps;

enum {
    VD_ATTACH_AUTH_PRIVATE_KEY_NON_EXPORTABLE = 1u << 0,
    VD_ATTACH_AUTH_PRIVATE_KEY_ISOLATED = 1u << 1,
    VD_ATTACH_AUTH_PRIVATE_KEY_RAW_SEED_IMPORT = 1u << 31,
};

/*
 * This backend owns all private key material.  No callback imports or exports
 * a seed/private key.  generate returns public material only.  destroy must
 * be idempotent for an already-absent key.
 */
typedef struct VdAttachAuthStorePrivateKeyOps {
    void *context;
    const void *trust_domain;
    uint32_t capabilities;
    int (*generate)(void *context, uint64_t key_id,
                    uint64_t generation,
                    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]);
    int (*load_public)(void *context, uint64_t key_id,
                       uint64_t generation,
                       uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]);
    int (*sign)(void *context, uint64_t key_id, uint64_t generation,
                const uint8_t *message, size_t message_size,
                uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
                uint64_t deadline_ms);
    int (*destroy)(void *context, uint64_t key_id,
                   uint64_t generation);
} VdAttachAuthStorePrivateKeyOps;

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
    const VdAttachAuthStorePersistenceOps *persistence,
    const VdAttachAuthStoreMonotonicOps *monotonic,
    const VdAttachAuthStorePrivateKeyOps *private_keys);

int vd_attach_auth_store_recover(VdAttachAuthStore *store);

/* Idempotent and non-destructive: only resident state/vtables are wiped. */
void vd_attach_auth_store_deinit(VdAttachAuthStore *store);

int vd_attach_auth_store_get_metadata(
    const VdAttachAuthStore *store,
    VdAttachAuthStoreMetadata *metadata);

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
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms);

int vd_attach_auth_store_provision(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation);

int vd_attach_auth_store_rotate(
    VdAttachAuthStore *store,
    uint64_t old_key_id,
    uint64_t old_generation,
    uint64_t new_key_id,
    uint64_t new_generation);

int vd_attach_auth_store_allow_peer(
    VdAttachAuthStore *store,
    const VdAttachAuthPublicKey *peer);

int vd_attach_auth_store_revoke_peer(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation);

/* Commits a tombstone before destroying the opaque local key. */
int vd_attach_auth_store_uninstall(VdAttachAuthStore *store);

/* Binds only the three listener-facing, non-administrative operations. */
int vd_attach_auth_store_bind(VdAttachAuthStore *store,
                              VdAttachAuthKeyStorage *storage);

/*
 * No public VitaSDK API can satisfy these trust requirements.  This sentinel
 * always wipes store and returns VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE.
 */
int vd_attach_auth_store_vita_init(VdAttachAuthStore *store);

#ifdef __cplusplus
}
#endif
