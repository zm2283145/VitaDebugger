#pragma once

#include <stddef.h>

#include <psp2/types.h>

#include "vitadebug_attach_auth_listener.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_VITA_NET_MEMORY_MIN (256u * 1024u)
#define VD_ATTACH_AUTH_VITA_WORKER_STACK (64u * 1024u)
#define VD_ATTACH_AUTH_VITA_STOP_TIMEOUT_US 3000000u

typedef struct VdAttachAuthVitaRuntime {
    VdAttachAuthListener *listener;
    void *network_memory;
    size_t network_memory_size;
    SceUID worker;
    int worker_started;
    int worker_ended;
    int network_module_loaded;
    int network_initialized;
} VdAttachAuthVitaRuntime;

/*
 * Initializes Vita networking owned by this runtime and supplies socket ops.
 * A future shell-resident integration must instead use a separately reviewed
 * shared-network owner; it must not call sceNetInit/sceNetTerm independently.
 */
int vd_attach_auth_vita_runtime_prepare(
    VdAttachAuthVitaRuntime *runtime,
    void *network_memory,
    size_t network_memory_size,
    VdAttachAuthSocketOps *socket_ops);

int vd_attach_auth_vita_runtime_start_worker(
    VdAttachAuthVitaRuntime *runtime,
    VdAttachAuthListener *listener);

/*
 * Cancels registered sockets, joins/deletes the worker, then releases owned
 * networking. Cleanup obligations are retained on failure and may be retried.
 */
int vd_attach_auth_vita_runtime_stop(
    VdAttachAuthVitaRuntime *runtime);

#ifdef __cplusplus
}
#endif
