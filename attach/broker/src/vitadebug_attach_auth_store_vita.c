#include "vitadebug_attach_auth_store.h"

int vd_attach_auth_store_vita_init(VdAttachAuthStore *store) {
    vd_attach_auth_store_deinit(store);
    return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
}
