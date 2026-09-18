#include "vitadebug_attach_auth.h"

#include <string.h>

/*
 * Device authentication is deliberately unavailable until an atomic,
 * permission-reviewed, persistent Vita key store is implemented and tested.
 */
void vd_attach_auth_vita_unavailable_storage(
    VdAttachAuthKeyStorage *storage) {
    if (storage != NULL) {
        memset(storage, 0, sizeof(*storage));
    }
}
