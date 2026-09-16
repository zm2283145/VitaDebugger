#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_broker.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-testable control-plane model for a future authenticated shell broker.
 *
 * This interface deliberately contains no socket implementation and no Vita
 * module-manager calls.  A production listener and privileged loader adapter
 * remain separate hardware-gated work.  In particular, there is no module
 * path in any caller-controlled structure: the backend receives only this
 * compiled-in slot identifier and must map it to one preinstalled, verified
 * debugger module.
 */
#define VD_ATTACH_CONTROL_VERSION 1u
#define VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT 0x56444431u /* "VDD1" */

#define VD_ATTACH_CONTROL_MAX_SESSIONS 4u
#define VD_ATTACH_CONTROL_MAX_AUTH_ATTEMPTS 3u
#define VD_ATTACH_CONTROL_MAX_REPLAY_NONCES 64u
#define VD_ATTACH_CONTROL_NONCE_BYTES 32u
#define VD_ATTACH_CONTROL_SIGNATURE_BYTES 64u

#define VD_ATTACH_CONTROL_MIN_LEASE_MS 250u
#define VD_ATTACH_CONTROL_MAX_LEASE_MS 60000u
#define VD_ATTACH_CONTROL_MIN_TIMEOUT_MS 100u
#define VD_ATTACH_CONTROL_MAX_TIMEOUT_MS 30000u
#define VD_ATTACH_CONTROL_MAX_AUTH_WINDOW_MS 60000u

enum {
    VD_ATTACH_CONTROL_OK = 0,
    VD_ATTACH_CONTROL_RECOVERY_REQUIRED = 1,
    VD_ATTACH_CONTROL_ERROR_ARGUMENT = -1,
    VD_ATTACH_CONTROL_ERROR_STATE = -2,
    VD_ATTACH_CONTROL_ERROR_AUTH = -3,
    VD_ATTACH_CONTROL_ERROR_EXPIRED = -4,
    VD_ATTACH_CONTROL_ERROR_REPLAY = -5,
    VD_ATTACH_CONTROL_ERROR_TARGET = -6,
    VD_ATTACH_CONTROL_ERROR_STALE_TARGET = -7,
    VD_ATTACH_CONTROL_ERROR_BUSY = -8,
    VD_ATTACH_CONTROL_ERROR_BACKEND = -9,
    VD_ATTACH_CONTROL_ERROR_ENTROPY = -10,
    VD_ATTACH_CONTROL_ERROR_SHUTDOWN = -11,
    VD_ATTACH_CONTROL_ERROR_LIMIT = -12,
    VD_ATTACH_CONTROL_ERROR_PEER = -13,
    /* Close this session, then open and authenticate a new connection. */
    VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED = -14,
};

typedef enum VdAttachControlOperation {
    VD_ATTACH_CONTROL_OPERATION_ATTACH = 1,
    VD_ATTACH_CONTROL_OPERATION_DETACH = 2,
    VD_ATTACH_CONTROL_OPERATION_RECOVER = 3,
} VdAttachControlOperation;

typedef enum VdAttachControlLeaseState {
    VD_ATTACH_CONTROL_LEASE_IDLE = 0,
    VD_ATTACH_CONTROL_LEASE_STARTING = 1,
    VD_ATTACH_CONTROL_LEASE_ACTIVE = 2,
    VD_ATTACH_CONTROL_LEASE_RECOVERY = 3,
} VdAttachControlLeaseState;

typedef struct VdAttachControlChallenge {
    uint64_t service_generation;
    uint64_t session_id;
    uint64_t transport_binding;
    uint64_t server_time_ms;
    uint64_t challenge_expires_at_ms;
    uint8_t server_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
} VdAttachControlChallenge;

/*
 * A paired-host verifier must authenticate this complete transcript. The
 * transport binding is opaque metadata supplied by the accepted transport and
 * is never an IP-address authorization policy by itself. Absolute times use
 * the broker's monotonic millisecond basis exposed by the challenge.
 */
typedef struct VdAttachControlPeerProof {
    uint64_t host_key_id;
    uint64_t expires_at_ms;
    uint8_t client_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
} VdAttachControlPeerProof;

typedef struct VdAttachControlPeerTranscript {
    uint32_t version;
    uint64_t service_generation;
    uint64_t session_id;
    uint64_t transport_binding;
    uint64_t host_key_id;
    uint64_t server_time_ms;
    uint64_t challenge_expires_at_ms;
    uint64_t expires_at_ms;
    uint8_t server_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t client_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
} VdAttachControlPeerTranscript;

/*
 * The host selects only a title, its previously observed nonzero generation,
 * and a bounded lease.  It cannot select a PID, module ID, module path, entry
 * point, or target address. expires_at_ms uses the challenge's broker time
 * basis and limits only authorization execution, not rollback of an acquired
 * lease.
 */
typedef struct VdAttachControlOperationProof {
    char target_title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    uint64_t expected_target_generation;
    uint32_t requested_lease_ms;
    uint64_t expires_at_ms;
    uint8_t request_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
} VdAttachControlOperationProof;

/*
 * Both the shell broker verifier and the future privileged loader receive the
 * same identity-bound authorization transcript.  The privileged boundary is
 * required to verify the signature and replay state independently.
 * Verifiers must serialize the named fields with a specified canonical wire
 * encoding; C structure bytes (including padding and native endianness) are
 * never a signature format.
 */
typedef struct VdAttachControlAuthorization {
    uint32_t version;
    uint32_t operation;
    uint32_t fixed_module_slot;
    uint64_t service_generation;
    uint64_t session_id;
    uint64_t transport_binding;
    uint64_t host_key_id;
    uint64_t expires_at_ms;
    uint64_t session_expires_at_ms;
    uint32_t requested_lease_ms;
    uint64_t lease_id;
    uint64_t lease_expires_at_ms;
    uint32_t injected_module_uid;
    VdAttachTargetIdentity target;
    uint8_t server_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t client_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t request_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
} VdAttachControlAuthorization;

typedef struct VdAttachControlFixedLoadRequest {
    VdAttachControlAuthorization authorization;
} VdAttachControlFixedLoadRequest;

/*
 * Allocated and returned by the privileged loader, never by the shell broker.
 * The loader must journal this exact grant atomically with acquisition. Its
 * expiry may not exceed either its own load-start time plus the signed
 * requested duration or the signed authenticated-session expiry.
 */
typedef struct VdAttachControlLeaseGrant {
    uint64_t lease_id;
    uint64_t lease_expires_at_ms;
    uint32_t injected_module_uid;
} VdAttachControlLeaseGrant;

typedef enum VdAttachControlJournalCapability {
    VD_ATTACH_CONTROL_JOURNAL_START = 1,
    VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP = 2,
    VD_ATTACH_CONTROL_JOURNAL_ROLLBACK = 3,
    VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED = 4,
    VD_ATTACH_CONTROL_JOURNAL_DISCONNECT = 5,
    VD_ATTACH_CONTROL_JOURNAL_SHUTDOWN = 6,
    VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY = 7,
} VdAttachControlJournalCapability;

typedef struct VdAttachControlModuleAction {
    uint32_t version;
    uint32_t fixed_module_slot;
    uint64_t service_generation;
    uint64_t lease_id;
    uint64_t lease_expires_at_ms;
    uint64_t owner_host_key_id;
    VdAttachTargetIdentity target;
    uint32_t injected_module_uid;
    uint32_t journal_capability;
    int has_signed_authorization;
    VdAttachControlAuthorization signed_authorization;
} VdAttachControlModuleAction;

typedef struct VdAttachControlLeaseSnapshot {
    uint32_t state;
    uint64_t lease_id;
    uint64_t expires_at_ms;
    uint64_t owner_host_key_id;
    VdAttachTargetIdentity target;
    uint32_t injected_module_uid;
    int module_loaded;
    int module_started;
} VdAttachControlLeaseSnapshot;

typedef enum VdAttachControlModulePresence {
    VD_ATTACH_CONTROL_MODULE_UNKNOWN = 0,
    VD_ATTACH_CONTROL_MODULE_PRESENT = 1,
    VD_ATTACH_CONTROL_MODULE_GONE = 2,
} VdAttachControlModulePresence;

/*
 * Return 1 only after verifying a paired-host signature over signed_bytes;
 * zero denies. The logical transcript is supplied for key lookup and audit,
 * but its native structure bytes are never a signing input.
 */
typedef int (*VdAttachControlVerifyPeerFn)(
    void *context, const VdAttachControlPeerTranscript *transcript,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms);

/* Return 1 only after verifying the operation signature over signed_bytes. */
typedef int (*VdAttachControlVerifyOperationFn)(
    void *context, const VdAttachControlAuthorization *authorization,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms);

typedef VdAttachInventoryResult (*VdAttachControlResolveTargetFn)(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms);

/*
 * These callbacks model the privileged boundary for host tests only.  The
 * load request has a fixed module slot and no path. A future Vita backend
 * must verify authorization, target identity/generation, module identity, and
 * replay state itself before acting. The privileged backend, not this core,
 * allocates and journals the lease grant. A failing load callback should not
 * retain a loaded resource; if it acquired one before failing, it must still
 * return the complete journaled grant so the core can request rollback. An
 * invalid/missing grant after reported success poisons the lease in recovery
 * state and blocks another attach.
 *
 * START requires a signed ATTACH authorization matching the exact journal.
 * HOST_CLEANUP requires the backend to independently verify the forwarded
 * DETACH/RECOVER signature and claim its operation nonce once for the whole
 * stop/unload transaction. The other capabilities authorize release-only
 * rollback of an exact existing journal record; they can never load, start,
 * retarget, or synthesize a lease. LEASE_EXPIRED additionally requires the
 * backend's monotonic clock to have reached the journaled expiry.
 * Stop must treat an already-stopped module as success because a failed start
 * is conservatively assumed to have executed partially.
 */
typedef int (*VdAttachControlLoadFixedFn)(
    void *context, const VdAttachControlFixedLoadRequest *request,
    VdAttachControlLeaseGrant *grant, uint64_t deadline_ms);
typedef int (*VdAttachControlModuleActionFn)(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);

/*
 * Read-only reconciliation for an exited/relaunched target.  GONE may be
 * returned only when the privileged provider proves that this exact
 * generation/lease/module resource no longer exists.  UNKNOWN/PRESENT retain
 * recovery ownership and never authorize a stop or unload against a new PID.
 */
typedef VdAttachControlModulePresence (*VdAttachControlProbeFixedFn)(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);

typedef struct VdAttachControlConfig {
    void *callback_context;
    VdAttachEntropyFn entropy;
    VdAttachNowMsFn now_ms;
    VdAttachControlVerifyPeerFn verify_peer;
    VdAttachControlVerifyOperationFn verify_operation;
    VdAttachControlResolveTargetFn resolve_target;
    VdAttachControlLoadFixedFn load_fixed;
    VdAttachControlModuleActionFn start_fixed;
    VdAttachControlModuleActionFn stop_fixed;
    VdAttachControlModuleActionFn unload_fixed;
    VdAttachControlProbeFixedFn probe_fixed;
    uint32_t challenge_timeout_ms;
    uint32_t auth_window_ms;
    uint32_t min_lease_ms;
    uint32_t max_lease_ms;
    uint32_t callback_timeout_ms;
    /* Optional dedicated context for the five fixed-loader callbacks. */
    void *loader_context;
} VdAttachControlConfig;

typedef struct VdAttachControlSession {
    int active;
    int authenticated;
    uint32_t auth_attempts;
    uint64_t session_id;
    uint64_t peer_id;
    uint64_t opened_at_ms;
    uint64_t auth_expires_at_ms;
    uint64_t host_key_id;
    uint8_t server_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t client_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    uint8_t operation_nonces[VD_ATTACH_CONTROL_MAX_REPLAY_NONCES]
                            [VD_ATTACH_CONTROL_NONCE_BYTES];
    uint32_t operation_nonce_count;
} VdAttachControlSession;

typedef struct VdAttachControlLease {
    uint32_t state;
    uint64_t lease_id;
    uint64_t expires_at_ms;
    uint64_t owner_session_id;
    uint64_t owner_host_key_id;
    VdAttachTargetIdentity target;
    uint32_t injected_module_uid;
    int module_loaded;
    int module_started;
} VdAttachControlLease;

typedef struct VdAttachControl {
    VdAttachControlConfig config;
    uint64_t service_generation;
    uint64_t next_session_id;
    int initialized;
    int shutting_down;
    VdAttachControlSession sessions[VD_ATTACH_CONTROL_MAX_SESSIONS];
    VdAttachControlLease lease;
} VdAttachControl;

/* Storage must be zero-initialized before the first init call. */
int vd_attach_control_init(VdAttachControl *control,
                           const VdAttachControlConfig *config);

int vd_attach_control_open_session(VdAttachControl *control,
                                   uint64_t peer_id,
                                   VdAttachControlChallenge *challenge);

int vd_attach_control_authenticate(VdAttachControl *control,
                                   uint64_t session_id,
                                   uint64_t peer_id,
                                   const VdAttachControlPeerProof *proof);

/*
 * Each authenticated session accepts at most MAX_REPLAY_NONCES operations.
 * ROLLOVER_REQUIRED is fail-closed: the caller must close the connection
 * (which rolls back any owned lease), then open and authenticate a fresh
 * session. No proof from the old session is valid in the replacement session.
 */
int vd_attach_control_begin_attach(
    VdAttachControl *control, uint64_t session_id, uint64_t peer_id,
    const VdAttachControlOperationProof *proof,
    VdAttachControlLeaseSnapshot *snapshot);

int vd_attach_control_detach(VdAttachControl *control,
                             uint64_t session_id,
                             uint64_t peer_id,
                             const VdAttachControlOperationProof *proof);

int vd_attach_control_recover(VdAttachControl *control,
                              uint64_t session_id,
                              uint64_t peer_id,
                              const VdAttachControlOperationProof *proof);

/* Watchdog entry point.  Expired leases are stopped then unloaded. */
int vd_attach_control_service(VdAttachControl *control);

/* Closing an owning session immediately attempts the same reverse rollback. */
int vd_attach_control_close_session(VdAttachControl *control,
                                    uint64_t session_id,
                                    uint64_t peer_id);

int vd_attach_control_snapshot(const VdAttachControl *control,
                               VdAttachControlLeaseSnapshot *snapshot);

/*
 * Invalidates sessions and prevents new operations. Repeated calls retry a
 * retained RECOVERY lease and are idempotent after cleanup succeeds.
 */
int vd_attach_control_shutdown(VdAttachControl *control);

#ifdef __cplusplus
}
#endif
