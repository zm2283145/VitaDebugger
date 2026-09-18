#include "vitadebug_attach_auth.h"

#include <string.h>

/*
 * Retail Vita authentication is deliberately unavailable until independently
 * reviewed opaque-key, handle-bound persistence, and monotonic backends exist.
 */
void vd_attach_auth_vita_unavailable_storage(
    VdAttachAuthKeyStorage *storage) {
    if (storage != NULL) {
        memset(storage, 0, sizeof(*storage));
    }
}
