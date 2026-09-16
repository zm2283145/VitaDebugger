#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_broker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_IDENTITY_MAX_TITLES 8u

typedef VdAttachInventoryResult (*VdAttachIdentityResolveTitleFn)(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    uint32_t *pid, uint64_t deadline_ms);

typedef VdAttachInventoryResult (*VdAttachIdentityReverseTitleFn)(
    void *context, uint32_t pid,
    char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    uint64_t deadline_ms);

/*
 * The trusted provider must obtain all fields from kernel-owned state and
 * assign a nonzero generation that changes across target lifetimes, including
 * rapid PID reuse. It must not accept identity fields from the network peer.
 */
typedef VdAttachInventoryResult (*VdAttachIdentitySnapshotFn)(
    void *context, uint32_t pid,
    const char expected_title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms);

typedef struct VdAttachIdentityConfig {
    void *callback_context;
    VdAttachNowMsFn now_ms;
    VdAttachIdentityResolveTitleFn resolve_title;
    VdAttachIdentityReverseTitleFn reverse_title;
    VdAttachIdentitySnapshotFn snapshot_trusted;
    const char (*allowed_title_ids)[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    size_t allowed_title_count;
} VdAttachIdentityConfig;

typedef struct VdAttachIdentityProvider {
    VdAttachIdentityConfig config;
    char allowed_title_ids[VD_ATTACH_IDENTITY_MAX_TITLES]
                          [VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    size_t allowed_title_count;
    int initialized;
} VdAttachIdentityProvider;

int vd_attach_identity_init(VdAttachIdentityProvider *provider,
                            const VdAttachIdentityConfig *config);

/* Direct adapter for broker/control/loader target-resolution callbacks. */
VdAttachInventoryResult vd_attach_identity_discover_exact(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms);

#ifdef __cplusplus
}
#endif
