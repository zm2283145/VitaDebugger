#include "vitadebug_attach_vita.h"

#include <string.h>

VdAttachInventoryResult vd_attach_vita_inventory_unavailable(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms) {
    (void)context;
    (void)title_id;
    (void)deadline_ms;
    if (identity != NULL) {
        memset(identity, 0, sizeof(*identity));
    }
    return VD_ATTACH_INVENTORY_UNAVAILABLE;
}
