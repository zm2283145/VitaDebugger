#pragma once

#include "vitadebug_attach_broker.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fail-closed Vita adapter for the current milestone.
 *
 * sceAppMgrGetIdByName()/sceAppMgrGetNameById() can perform the preliminary
 * exact-title check, but the public user API cannot independently provide the
 * required foreign main-module ID, fingerprint, and target generation.  Until
 * a separately reviewed read-only identity provider exists, this adapter never
 * produces a ticket.
 */
VdAttachInventoryResult vd_attach_vita_inventory_unavailable(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms);

#ifdef __cplusplus
}
#endif
