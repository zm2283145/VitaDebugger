#include "vitadebug_attach_auth_listener_vita.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

static uint64_t vd_auth_vita_now_ms(void *context) {
    (void)context;
    return (uint64_t)sceKernelGetSystemTimeWide() / 1000u;
}

static int vd_auth_vita_entropy(void *context,
                                uint8_t *output,
                                size_t size) {
    (void)context;
    if (output == NULL || size == 0u || size > UINT_MAX) {
        return -1;
    }
    return sceKernelGetRandomNumber(output, (SceSize)size);
}

static int vd_auth_vita_would_block(void) {
    int *error = sceNetErrnoLoc();
    return error != NULL &&
           (*error == SCE_NET_EAGAIN ||
            *error == SCE_NET_EWOULDBLOCK);
}

static int vd_auth_vita_wait_until(uint64_t deadline_ms) {
    if (vd_auth_vita_now_ms(NULL) >= deadline_ms) {
        return VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT;
    }
    sceKernelDelayThread(1000u);
    return VD_ATTACH_AUTH_LISTENER_OK;
}

static int vd_auth_vita_listen_open(void *context,
                                    uint32_t bind_ipv4,
                                    uint16_t port,
                                    int *socket_out) {
    SceNetSockaddrIn address;
    int reuse = 1;
    int nonblocking = 1;
    int socket;
    (void)context;
    if (socket_out == NULL) {
        return -1;
    }
    *socket_out = -1;
    socket = sceNetSocket("VdAttachAuth", SCE_NET_AF_INET,
                          SCE_NET_SOCK_STREAM, 0);
    if (socket < 0) {
        return socket;
    }
    if (sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET,
                         SCE_NET_SO_REUSEADDR, &reuse,
                         sizeof(reuse)) < 0 ||
        sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                         &nonblocking, sizeof(nonblocking)) < 0) {
        (void)sceNetSocketClose(socket);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(port);
    address.sin_addr.s_addr = sceNetHtonl(bind_ipv4);
    if (sceNetBind(socket, (const SceNetSockaddr *)&address,
                   sizeof(address)) < 0 ||
        sceNetListen(socket, 1) < 0) {
        (void)sceNetSocketClose(socket);
        return -1;
    }
    *socket_out = socket;
    return 0;
}

static int vd_auth_vita_accept(void *context,
                               int listen_socket,
                               int *socket_out,
                               uint32_t *peer_ipv4,
                               uint64_t deadline_ms) {
    SceNetSockaddrIn peer;
    unsigned int peer_size;
    int socket;
    (void)context;
    if (socket_out == NULL || peer_ipv4 == NULL) {
        return -1;
    }
    *socket_out = -1;
    *peer_ipv4 = 0u;
    for (;;) {
        memset(&peer, 0, sizeof(peer));
        peer_size = sizeof(peer);
        socket = sceNetAccept(listen_socket, (SceNetSockaddr *)&peer,
                              &peer_size);
        if (socket >= 0) {
            int nonblocking = 1;
            if (peer_size != sizeof(peer) ||
                peer.sin_family != SCE_NET_AF_INET ||
                sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET,
                                 SCE_NET_SO_NBIO, &nonblocking,
                                 sizeof(nonblocking)) < 0) {
                (void)sceNetSocketClose(socket);
                return -1;
            }
            *peer_ipv4 = sceNetNtohl(peer.sin_addr.s_addr);
            *socket_out = socket;
            return VD_ATTACH_AUTH_LISTENER_OK;
        }
        if (!vd_auth_vita_would_block()) {
            return socket;
        }
        if (vd_auth_vita_wait_until(deadline_ms) !=
            VD_ATTACH_AUTH_LISTENER_OK) {
            return VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT;
        }
    }
}

static int vd_auth_vita_read(void *context,
                             int socket,
                             void *output,
                             size_t size,
                             uint64_t deadline_ms) {
    int result;
    (void)context;
    if (output == NULL || size == 0u || size > UINT_MAX) {
        return -1;
    }
    for (;;) {
        result = sceNetRecv(socket, output, (unsigned int)size,
                            SCE_NET_MSG_DONTWAIT);
        if (result >= 0) {
            return result;
        }
        if (!vd_auth_vita_would_block()) {
            return result;
        }
        if (vd_auth_vita_wait_until(deadline_ms) !=
            VD_ATTACH_AUTH_LISTENER_OK) {
            return VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT;
        }
    }
}

static int vd_auth_vita_write(void *context,
                              int socket,
                              const void *data,
                              size_t size,
                              uint64_t deadline_ms) {
    int result;
    (void)context;
    if (data == NULL || size == 0u || size > UINT_MAX) {
        return -1;
    }
    for (;;) {
        result = sceNetSend(socket, data, (unsigned int)size,
                            SCE_NET_MSG_DONTWAIT);
        if (result >= 0) {
            return result;
        }
        if (!vd_auth_vita_would_block()) {
            return result;
        }
        if (vd_auth_vita_wait_until(deadline_ms) !=
            VD_ATTACH_AUTH_LISTENER_OK) {
            return VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT;
        }
    }
}

static int vd_auth_vita_shutdown(void *context, int socket) {
    (void)context;
    return sceNetShutdown(socket, SCE_NET_SHUT_RDWR);
}

static int vd_auth_vita_close(void *context, int socket) {
    (void)context;
    return sceNetSocketClose(socket);
}

static int vd_auth_vita_worker(SceSize argument_size, void *arguments) {
    VdAttachAuthVitaRuntime *runtime = NULL;
    int result;
    if (argument_size != sizeof(runtime) || arguments == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    memcpy(&runtime, arguments, sizeof(runtime));
    if (runtime == NULL || runtime->listener == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    result = vd_attach_auth_listener_run(runtime->listener);
    return result;
}

int vd_attach_auth_vita_runtime_prepare(
    VdAttachAuthVitaRuntime *runtime,
    void *network_memory,
    size_t network_memory_size,
    VdAttachAuthSocketOps *socket_ops) {
    SceNetInitParam init;
    int result;
    if (runtime == NULL || socket_ops == NULL ||
        network_memory == NULL ||
        network_memory_size < VD_ATTACH_AUTH_VITA_NET_MEMORY_MIN ||
        network_memory_size > INT_MAX) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->worker = -1;
    runtime->network_memory = network_memory;
    runtime->network_memory_size = network_memory_size;
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0) {
        return result;
    }
    runtime->network_module_loaded = 1;
    memset(&init, 0, sizeof(init));
    init.memory = network_memory;
    init.size = (int)network_memory_size;
    result = sceNetInit(&init);
    if (result < 0) {
        if (sceSysmoduleUnloadModule(SCE_SYSMODULE_NET) >= 0) {
            runtime->network_module_loaded = 0;
            return result;
        }
        return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
    }
    runtime->network_initialized = 1;
    memset(socket_ops, 0, sizeof(*socket_ops));
    socket_ops->context = runtime;
    socket_ops->now_ms = vd_auth_vita_now_ms;
    socket_ops->entropy = vd_auth_vita_entropy;
    socket_ops->listen_open = vd_auth_vita_listen_open;
    socket_ops->accept = vd_auth_vita_accept;
    socket_ops->read = vd_auth_vita_read;
    socket_ops->write = vd_auth_vita_write;
    socket_ops->shutdown = vd_auth_vita_shutdown;
    socket_ops->close = vd_auth_vita_close;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

int vd_attach_auth_vita_runtime_start_worker(
    VdAttachAuthVitaRuntime *runtime,
    VdAttachAuthListener *listener) {
    VdAttachAuthVitaRuntime *argument;
    int result;
    if (runtime == NULL || listener == NULL ||
        !runtime->network_initialized || runtime->worker >= 0 ||
        listener->state != VD_ATTACH_AUTH_LISTENER_RUNNING) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_STATE;
    }
    runtime->listener = listener;
    runtime->worker = sceKernelCreateThread(
        "VdAttachAuthWorker", vd_auth_vita_worker, 0x10000100,
        VD_ATTACH_AUTH_VITA_WORKER_STACK, 0,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
    if (runtime->worker < 0) {
        result = runtime->worker;
        runtime->worker = -1;
        runtime->listener = NULL;
        return result;
    }
    argument = runtime;
    result = sceKernelStartThread(runtime->worker, sizeof(argument),
                                  &argument);
    if (result < 0) {
        int delete_result = sceKernelDeleteThread(runtime->worker);
        if (delete_result >= 0) {
            runtime->worker = -1;
            runtime->listener = NULL;
        }
        return result;
    }
    runtime->worker_started = 1;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

int vd_attach_auth_vita_runtime_stop(
    VdAttachAuthVitaRuntime *runtime) {
    int result = VD_ATTACH_AUTH_LISTENER_OK;
    if (runtime == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    if (runtime->listener != NULL) {
        if (vd_attach_auth_listener_shutdown(runtime->listener) < 0) {
            result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
        }
    }
    if (runtime->worker >= 0) {
        if (runtime->worker_started && !runtime->worker_ended) {
            SceUInt timeout = VD_ATTACH_AUTH_VITA_STOP_TIMEOUT_US;
            int status = 0;
            if (sceKernelWaitThreadEnd(runtime->worker, &status,
                                       &timeout) < 0) {
                return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
            }
            runtime->worker_ended = 1;
        }
        if (sceKernelDeleteThread(runtime->worker) < 0) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
        }
        runtime->worker = -1;
        runtime->worker_started = 0;
        runtime->worker_ended = 0;
        runtime->listener = NULL;
    }
    if (runtime->network_initialized) {
        if (sceNetTerm() < 0) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
        }
        runtime->network_initialized = 0;
    }
    if (runtime->network_module_loaded) {
        if (sceSysmoduleUnloadModule(SCE_SYSMODULE_NET) < 0) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
        }
        runtime->network_module_loaded = 0;
    }
    return result;
}
