#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-testable privileged-backend contract.
 *
 * It maps the one public debugger slot to one trusted startup descriptor,
 * keeps a bounded lease/replay journal, and validates control actions before
 * invoking injected platform callbacks.  This file contains no Vita module
 * manager imports and ships no production module path or digest.
 */
#define VD_ATTACH_LOADER_VERSION 1u
#define VD_ATTACH_LOADER_MAX_TITLES 8u
#define VD_ATTACH_LOADER_MAX_PATH_BYTES 128u
#define VD_ATTACH_LOADER_DIGEST_BYTES 32u
#define VD_ATTACH_LOADER_MAX_REPLAY_NONCES 64u
#define VD_ATTACH_LOADER_MAX_LEASE_IDS 64u

enum {
    VD_ATTACH_LOADER_OK = 0,
    VD_ATTACH_LOADER_ERROR_ARGUMENT = -1,
    VD_ATTACH_LOADER_ERROR_STATE = -2,
    VD_ATTACH_LOADER_ERROR_AUTH = -3,
    VD_ATTACH_LOADER_ERROR_REPLAY = -4,
    VD_ATTACH_LOADER_ERROR_TARGET = -5,
    VD_ATTACH_LOADER_ERROR_MODULE = -6,
    VD_ATTACH_LOADER_ERROR_PLATFORM = -7,
    VD_ATTACH_LOADER_ERROR_EXPIRED = -8,
    VD_ATTACH_LOADER_ERROR_ENTROPY = -9,
    VD_ATTACH_LOADER_ERROR_LIMIT = -10,
};

typedef enum VdAttachLoaderJournalState {
    VD_ATTACH_LOADER_JOURNAL_IDLE = 0,
    VD_ATTACH_LOADER_JOURNAL_LOADING = 1,
    VD_ATTACH_LOADER_JOURNAL_LOADED = 2,
    VD_ATTACH_LOADER_JOURNAL_START_MAYBE = 3,
    VD_ATTACH_LOADER_JOURNAL_STARTED = 4,
    VD_ATTACH_LOADER_JOURNAL_STOPPED = 5,
    VD_ATTACH_LOADER_JOURNAL_RECOVERY = 6,
} VdAttachLoaderJournalState;

typedef struct VdAttachFixedModuleDescriptor {
    uint32_t fixed_module_slot;
    char canonical_path[VD_ATTACH_LOADER_MAX_PATH_BYTES];
    uint8_t sha256[VD_ATTACH_LOADER_DIGEST_BYTES];
} VdAttachFixedModuleDescriptor;

typedef int (*VdAttachLoaderVerifyModuleFn)(
    void *context, const VdAttachFixedModuleDescriptor *module,
    uint64_t deadline_ms);

/*
 * load_module must leave module_uid zero if no resource was acquired. If a
 * failed call may have loaded a module, it must return that positive UID so
 * the journal and control core can perform rollback.
 */
typedef int (*VdAttachLoaderLoadModuleFn)(
    void *context, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t *module_uid,
    uint64_t deadline_ms);

typedef int (*VdAttachLoaderModuleActionFn)(
    void *context, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms);

typedef VdAttachControlModulePresence (*VdAttachLoaderProbeModuleFn)(
    void *context, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms);

typedef struct VdAttachFixedLoaderConfig {
    void *callback_context;
    VdAttachNowMsFn now_ms;
    VdAttachEntropyFn entropy;
    VdAttachControlVerifyOperationFn verify_authorization;
    VdAttachControlResolveTargetFn resolve_target;
    VdAttachLoaderVerifyModuleFn verify_module;
    VdAttachLoaderLoadModuleFn load_module;
    VdAttachLoaderModuleActionFn start_module;
    VdAttachLoaderModuleActionFn stop_module;
    VdAttachLoaderModuleActionFn unload_module;
    VdAttachLoaderProbeModuleFn probe_module;
    const char (*allowed_title_ids)[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    size_t allowed_title_count;
    VdAttachFixedModuleDescriptor debugger_module;
    uint32_t min_lease_ms;
    uint32_t max_lease_ms;
} VdAttachFixedLoaderConfig;

typedef struct VdAttachLoaderJournal {
    uint32_t state;
    uint64_t service_generation;
    uint64_t owner_host_key_id;
    VdAttachTargetIdentity target;
    VdAttachControlLeaseGrant grant;
    int module_loaded;
    int module_started;
    int start_may_have_run;
    VdAttachControlAuthorization attach_authorization;
    int cleanup_authorization_active;
    VdAttachControlAuthorization cleanup_authorization;
} VdAttachLoaderJournal;

typedef struct VdAttachFixedLoader {
    VdAttachFixedLoaderConfig config;
    char allowed_title_ids[VD_ATTACH_LOADER_MAX_TITLES]
                          [VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    size_t allowed_title_count;
    uint64_t service_generation;
    uint8_t operation_nonces[VD_ATTACH_LOADER_MAX_REPLAY_NONCES]
                             [VD_ATTACH_CONTROL_NONCE_BYTES];
    uint32_t operation_nonce_count;
    uint64_t lease_ids[VD_ATTACH_LOADER_MAX_LEASE_IDS];
    uint32_t lease_id_count;
    VdAttachLoaderJournal journal;
    int initialized;
} VdAttachFixedLoader;

typedef struct VdAttachLoaderSnapshot {
    uint32_t state;
    uint64_t service_generation;
    uint64_t owner_host_key_id;
    VdAttachTargetIdentity target;
    VdAttachControlLeaseGrant grant;
    int module_loaded;
    int module_started;
    int start_may_have_run;
    uint32_t replay_nonce_count;
} VdAttachLoaderSnapshot;

/* Storage must be zero-initialized before first initialization. */
int vd_attach_fixed_loader_init(VdAttachFixedLoader *loader,
                                const VdAttachFixedLoaderConfig *config);

/* Bind exactly once to the generation created by VdAttachControl. */
int vd_attach_fixed_loader_bind_service_generation(
    VdAttachFixedLoader *loader, uint64_t service_generation);

/* Direct adapters for VdAttachControlConfig's loader callbacks. */
int vd_attach_fixed_loader_load(
    void *context, const VdAttachControlFixedLoadRequest *request,
    VdAttachControlLeaseGrant *grant, uint64_t deadline_ms);
int vd_attach_fixed_loader_start(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);
int vd_attach_fixed_loader_stop(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);
int vd_attach_fixed_loader_unload(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);
VdAttachControlModulePresence vd_attach_fixed_loader_probe(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms);

int vd_attach_fixed_loader_snapshot(const VdAttachFixedLoader *loader,
                                    VdAttachLoaderSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
