#pragma once

/* Minimal host declarations used only when test_uvdb_core_integration.c
 * includes the production uvdb.c translation unit. They model ABI shapes
 * needed to compile the real protocol/lifecycle code; fake implementations
 * live in the test and no VitaSDK object is linked. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int SceUID;
typedef unsigned int SceSize;

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

static inline uint16_t htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x80
#define SHUT_RDWR 2
#ifndef SIGTRAP
#define SIGTRAP 5
#endif

typedef struct SceKernelAddrPair {
    uintptr_t addr;
    size_t length;
} SceKernelAddrPair;

typedef struct SceKernelModuleSegmentInfo {
    void* vaddr;
    size_t memsz;
    unsigned int perms;
} SceKernelModuleSegmentInfo;

typedef struct SceKernelModuleInfo {
    size_t size;
    char module_name[28];
    SceKernelModuleSegmentInfo segments[4];
} SceKernelModuleInfo;

typedef struct SceNetEpollEvent {
    unsigned int events;
    union {
        int fd;
        uintptr_t value;
    } data;
} SceNetEpollEvent;

typedef struct KuKernelExceptionContext {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t SPSR;
    uint32_t exceptionType;
    uint32_t FSR;
    uint32_t FAR;
} KuKernelExceptionContext;

typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext* context);

struct KuKernelExceptionHandlerOpt {
    size_t size;
};

#define KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT 0u
#define KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT 1u
#define KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION 2u

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0
#define SCE_NET_EPOLL_CTL_ADD 1
#define SCE_NET_EPOLLIN 0x01u
#define SCE_NET_EPOLLERR 0x08u
#define SCE_NET_EPOLLHUP 0x10u
#define SCE_NET_MSG_DONTWAIT 0x80
#define SCE_NET_ERROR_EAGAIN ((int32_t)UINT32_C(0x80410123))
#define SCE_NET_EAGAIN 35
#define SCE_NET_EWOULDBLOCK 35

SceUID sceKernelAllocMemBlock(
    const char* name, int type, size_t size, void* options);
int sceKernelGetMemBlockBase(SceUID uid, void** base);
int sceKernelFreeMemBlock(SceUID uid);
int sceKernelDelayThread(unsigned int microseconds);
SceUID sceKernelGetThreadId(void);
SceUID sceKernelCreateMsgPipe(
    const char* name, int attributes, int unknown, size_t size,
    void* options);
int sceKernelDeleteMsgPipe(SceUID uid);
SceUID sceKernelCreateThread(
    const char* name, int (*entry)(SceSize, void*), int priority,
    size_t stack_size, unsigned int attributes, int affinity,
    void* options);
int sceKernelStartThread(SceUID uid, SceSize args, void* argp);
int sceKernelWaitThreadEnd(SceUID uid, int* status, void* timeout);
int sceKernelDeleteThread(SceUID uid);
SceUID sceKernelGetModuleIdByAddr(const void* address);
int sceKernelGetModuleInfo(SceUID uid, SceKernelModuleInfo* info);
int sceKernelGetModuleList(int flags, SceUID* modules, SceSize* count);

int sceNetSyscallSocket(
    const char* name, int domain, int type, int protocol);
int sceNetSyscallSetsockopt(void* arguments);
int sceNetSyscallBind(int socket, const void* address, unsigned int size);
int sceNetSyscallListen(int socket, int backlog);
int sceNetSyscallAccept(int socket, void* address, void* address_size);
ssize_t sceNetSyscallRecvfrom(void* arguments);
ssize_t sceNetSyscallSendto(void* arguments);
int sceNetSyscallShutdown(int socket, int how);
int sceNetSyscallSocketAbort(int socket, int flags);
int sceNetSyscallClose(int socket);
int* sceNetErrnoLoc(void);
int sceNetSend(int socket, const void* data, size_t size, int flags);
int sceNetEpollCreate(const char* name, int flags);
int sceNetEpollControl(
    int epoll, int operation, int socket, SceNetEpollEvent* event);
int sceNetEpollWait(
    int epoll, SceNetEpollEvent* events, int maximum, int timeout);
int sceNetEpollDestroy(int epoll);

int kuKernelCpuUnrestrictedMemcpy(void* destination,
                                  const void* source, size_t size);
void kuKernelFlushCaches(const void* address, size_t size);
int kuKernelRegisterExceptionHandler(
    uint32_t exception_type, KuKernelExceptionHandler replacement,
    KuKernelExceptionHandler* previous,
    struct KuKernelExceptionHandlerOpt* options);
void kuKernelReleaseExceptionHandler(uint32_t exception_type);
