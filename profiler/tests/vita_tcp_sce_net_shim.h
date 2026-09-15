#ifndef VITA_TCP_SCE_NET_SHIM_H
#define VITA_TCP_SCE_NET_SHIM_H

#include <stdint.h>

#define SCE_NET_AF_INET 2
#define SCE_NET_SOCK_STREAM 1
#define SCE_NET_IPPROTO_TCP 6
#define SCE_NET_SOL_SOCKET 0xffff
#define SCE_NET_SO_ERROR 0x1007
#define SCE_NET_SO_NBIO 0x1100
#define SCE_NET_MSG_DONTWAIT 0x00000080
#define SCE_NET_SHUT_WR 1

#define SCE_NET_EPOLL_CTL_ADD 1
#define SCE_NET_EPOLLOUT UINT32_C(0x00000002)
#define SCE_NET_EPOLLERR UINT32_C(0x00000008)
#define SCE_NET_EPOLLHUP UINT32_C(0x00000010)

#define SCE_NET_ERROR_EINPROGRESS UINT32_C(0x80410124)
#define SCE_NET_ERROR_EALREADY UINT32_C(0x80410125)
#define SCE_NET_ERROR_EISCONN UINT32_C(0x80410138)
#define SCE_NET_ERROR_EAGAIN UINT32_C(0x80410123)
#define SCE_NET_EAGAIN 35
#define SCE_NET_EINPROGRESS 36
#define SCE_NET_EALREADY 37
#define SCE_NET_EISCONN 56
#define SCE_NET_ENOTCONN 57

typedef struct SceNetInAddr {
    unsigned int s_addr;
} SceNetInAddr;

typedef struct SceNetSockaddrIn {
    unsigned char sin_len;
    unsigned char sin_family;
    unsigned short sin_port;
    SceNetInAddr sin_addr;
    unsigned short sin_vport;
    char sin_zero[6];
} SceNetSockaddrIn;

typedef struct SceNetSockaddr {
    unsigned char sa_len;
    unsigned char sa_family;
    char sa_data[14];
} SceNetSockaddr;

typedef union SceNetEpollData {
    void* ptr;
    int fd;
    unsigned int u32;
    unsigned long long u64;
} SceNetEpollData;

typedef struct SceNetEpollEvent {
    unsigned int events;
    unsigned int reserved;
    unsigned int system[4];
    SceNetEpollData data;
} SceNetEpollEvent;

struct vp_test_sce_net_state {
    uint64_t now_us;
    int socket_result;
    int setsockopt_result;
    int epoll_create_result;
    int epoll_control_result;
    int connect_result;
    int connect_errno;
    int getsockopt_result;
    int socket_error;
    int epoll_wait_result;
    unsigned int epoll_wait_events;
    int send_result;
    int send_returns_length;
    int shutdown_result;
    int epoll_destroy_failures;
    int socket_close_failures;
    int errno_value;
    int last_wait_timeout_us;
    uint32_t socket_calls;
    uint32_t epoll_create_calls;
    uint32_t epoll_destroy_calls;
    uint32_t socket_close_calls;
    uint32_t shutdown_calls;
    uint32_t send_calls;
};

void vp_test_sce_net_reset(void);
struct vp_test_sce_net_state* vp_test_sce_net_get_state(void);

uint64_t sceKernelGetProcessTimeWide(void);
int* sceNetErrnoLoc(void);
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSetsockopt(int socket, int level, int option, const void* value,
                     unsigned int size);
int sceNetEpollCreate(const char* name, int flags);
int sceNetEpollControl(int epoll, int operation, int socket,
                       SceNetEpollEvent* event);
unsigned short sceNetHtons(unsigned short value);
unsigned int sceNetHtonl(unsigned int value);
int sceNetConnect(int socket, const SceNetSockaddr* address,
                  unsigned int size);
int sceNetGetsockopt(int socket, int level, int option, void* value,
                     unsigned int* size);
int sceNetEpollWait(int epoll, SceNetEpollEvent* events, int max_events,
                    int timeout_us);
int sceNetSend(int socket, const void* data, unsigned int size, int flags);
int sceNetShutdown(int socket, int how);
int sceNetEpollDestroy(int epoll);
int sceNetSocketClose(int socket);

#endif
