#include "vita_tcp_sce_net_shim.h"

#include <limits.h>
#include <string.h>

static struct vp_test_sce_net_state state;

void vp_test_sce_net_reset(void)
{
    memset(&state, 0, sizeof(state));
    state.now_us = UINT64_C(100000);
    state.socket_result = 31;
    state.epoll_create_result = 41;
    state.epoll_wait_result = 1;
    state.epoll_wait_events = SCE_NET_EPOLLOUT;
    state.send_returns_length = 1;
}

struct vp_test_sce_net_state* vp_test_sce_net_get_state(void)
{
    return &state;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    return state.now_us;
}

int* sceNetErrnoLoc(void)
{
    return &state.errno_value;
}

int sceNetSocket(const char* name, int domain, int type, int protocol)
{
    (void)name;
    (void)domain;
    (void)type;
    (void)protocol;
    ++state.socket_calls;
    return state.socket_result;
}

int sceNetSetsockopt(int socket, int level, int option, const void* value,
                     unsigned int size)
{
    (void)socket;
    (void)level;
    (void)option;
    (void)value;
    (void)size;
    if (state.setsockopt_result < 0)
        state.errno_value = -601;
    return state.setsockopt_result;
}

int sceNetEpollCreate(const char* name, int flags)
{
    (void)name;
    (void)flags;
    ++state.epoll_create_calls;
    return state.epoll_create_result;
}

int sceNetEpollControl(int epoll, int operation, int socket,
                       SceNetEpollEvent* event)
{
    (void)epoll;
    (void)operation;
    (void)socket;
    (void)event;
    if (state.epoll_control_result < 0)
        state.errno_value = -602;
    return state.epoll_control_result;
}

unsigned short sceNetHtons(unsigned short value)
{
    return (unsigned short)((value << 8) | (value >> 8));
}

unsigned int sceNetHtonl(unsigned int value)
{
    return ((value & UINT32_C(0x000000ff)) << 24) |
           ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) |
           ((value & UINT32_C(0xff000000)) >> 24);
}

int sceNetConnect(int socket, const SceNetSockaddr* address,
                  unsigned int size)
{
    (void)socket;
    (void)address;
    (void)size;
    if (state.connect_result < 0)
        state.errno_value = state.connect_errno;
    return state.connect_result;
}

int sceNetGetsockopt(int socket, int level, int option, void* value,
                     unsigned int* size)
{
    (void)socket;
    (void)level;
    (void)option;
    if (state.getsockopt_result < 0)
        return state.getsockopt_result;
    if (value != NULL && size != NULL && *size >= sizeof(int)) {
        *(int*)value = state.socket_error;
        *size = (unsigned int)sizeof(int);
    }
    return state.getsockopt_result;
}

int sceNetEpollWait(int epoll, SceNetEpollEvent* events, int max_events,
                    int timeout_us)
{
    (void)epoll;
    state.last_wait_timeout_us = timeout_us;
    if (state.epoll_wait_result > 0 && events != NULL && max_events > 0) {
        memset(events, 0, sizeof(*events));
        events->events = state.epoll_wait_events;
        events->data.fd = state.socket_result;
    }
    return state.epoll_wait_result;
}

int sceNetSend(int socket, const void* data, unsigned int size, int flags)
{
    (void)socket;
    (void)data;
    (void)flags;
    ++state.send_calls;
    if (state.send_returns_length)
        return size <= (unsigned int)INT_MAX ? (int)size : INT_MAX;
    return state.send_result;
}

int sceNetShutdown(int socket, int how)
{
    (void)socket;
    (void)how;
    ++state.shutdown_calls;
    return state.shutdown_result;
}

int sceNetEpollDestroy(int epoll)
{
    (void)epoll;
    ++state.epoll_destroy_calls;
    if (state.epoll_destroy_failures > 0) {
        --state.epoll_destroy_failures;
        state.errno_value = -701;
        return -1;
    }
    return 0;
}

int sceNetSocketClose(int socket)
{
    (void)socket;
    ++state.socket_close_calls;
    if (state.socket_close_failures > 0) {
        --state.socket_close_failures;
        state.errno_value = -702;
        return -1;
    }
    return 0;
}
