#pragma once

typedef struct SceNetSyscallParameter SceNetSyscallParameter;

int sceNetSyscallSocket(const char* name, int domain, int type, int protocol);
int sceNetSyscallShutdown(int socket, int how);
int sceNetSyscallClose(int socket);
int sceNetSyscallSendto(SceNetSyscallParameter* parameters);
