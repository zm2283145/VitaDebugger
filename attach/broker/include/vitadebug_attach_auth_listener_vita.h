#pragma once

#include <stddef.h>

#include <psp2/types.h>

#include "vitadebug_attach_auth_listener.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_VITA_NET_MEMORY_MIN \
    VD_ATTACH_AUTH_NETWORK_MEMORY_MIN
#define VD_ATTACH_AUTH_VITA_WORKER_STACK (64u * 1024u)
#define VD_ATTACH_AUTH_VITA_STOP_TIMEOUT_US 3000000u

typedef struct VdAttachAuthVitaRuntime {
    VdAttachAuthListener *listener;
    void *network_memory;
    size_t network_memory_size;
    SceUID worker;
    int worker_started;
    int worker_ended;
    VdAttachAuthNetworkMode network_mode;
    int network_ready;
    int network_module_loaded;
    int network_initialized;
} VdAttachAuthVitaRuntime;

/*
 * Loads and initializes Vita networking owned exclusively by a disposable
 * title. The matching stop path terminates and unloads only these resources.
 */
int vd_attach_auth_vita_runtime_prepare_standalone(
    VdAttachAuthVitaRuntime *runtime,
    void *network_memory,
    size_t network_memory_size,
    VdAttachAuthSocketOps *socket_ops);

/*
 * Borrows already-ready shell networking. This performs no SceNet or NetCtl
 * initialization/transition and stop never calls sceNetTerm or unloads the
 * shared network module.
 */
int vd_attach_auth_vita_runtime_prepare_borrowed(
    VdAttachAuthVitaRuntime *runtime,
    VdAttachAuthSocketOps *socket_ops);

int vd_attach_auth_vita_runtime_start_worker(
    VdAttachAuthVitaRuntime *runtime,
    VdAttachAuthListener *listener);

/*
 * Cancels registered sockets and joins/deletes the worker. It then releases
 * networking only for standalone-owned mode. Borrowed mode never terminates
 * SceNet or unloads shared modules. Cleanup obligations are retained on
 * failure and may be retried.
 */
int vd_attach_auth_vita_runtime_stop(
    VdAttachAuthVitaRuntime *runtime);

#ifdef __cplusplus
}
#endif
