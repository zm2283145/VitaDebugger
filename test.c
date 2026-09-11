#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "debugScreen.h"
#include "uvdb.h"

static volatile int test_value;
volatile int trigger_fault;

__attribute__((noinline)) static int step_target(int value)
{
    value += 3;
    if(value & 1)
        value *= 2;
    else
        value -= 1;
    return value;
}

int main(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(0x08080808)},
        .sin_port = htons(53),
    };
    connect(sock, (void*)&sin, sizeof(sin));
    socklen_t l = sizeof(sin);
    getsockname(sock, (void*)&sin, &l);
    close(sock);
    psvDebugScreenInit();
    uint8_t addr[4];
    memcpy(addr, &sin.sin_addr.s_addr, 4);
    psvDebugScreenPrintf("Run the following command on your PC:\n");
    psvDebugScreenPrintf("$ gdb test.elf -ex 'target remote %hhu.%hhu.%hhu.%hhu:1234'\n", addr[0], addr[1], addr[2], addr[3]);
    uvdb_enter();
    psvDebugScreenPrintf("Debugger connected. Running quiet step target.\n");
    for(int i = 0;; i++)
    {
        if(trigger_fault)
            *(volatile unsigned int*)0 = 0x55464442;
        test_value = step_target(i);
        if((i % 10) == 0)
            psvDebugScreenPrintf("alive: i=%d value=%d\n", i, test_value);
        usleep(100000);
    }
}
