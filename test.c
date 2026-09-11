#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include "debugScreen.h"
#include "uvdb.h"

static volatile int test_value;
volatile int trigger_fault;
static volatile int worker_values[2];

void thumb_step_pop_fixture(void);
void thumb_step_mov_fixture(void);
void thumb_step_tbb_fixture(void);
void thumb_step_tbh_fixture(void);
void thumb_step_ldm_fixture(void);
void thumb_step_it_fixture(void);
void thumb_step_ldmdb_fixture(void);
void arm_step_mov_fixture(void);
void arm_step_ldm_fixture(void);

static void* worker_main(void* argument)
{
    intptr_t index = (intptr_t)argument;
    uvdb_register_thread(index == 0 ? "test worker 0" : "test worker 1");
    for(;;)
    {
        worker_values[index]++;
        usleep(20000 + (unsigned int)index * 10000);
    }
    uvdb_unregister_thread();
    return NULL;
}

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
    uvdb_register_thread("test main");
    pthread_t workers[2];
    pthread_create(&workers[0], NULL, worker_main, (void*)0);
    pthread_create(&workers[1], NULL, worker_main, (void*)1);
    if(uvdb_start_server() < 0)
    {
        psvDebugScreenPrintf("Failed to start persistent debugger server.\n");
        return 1;
    }
    psvDebugScreenPrintf("Persistent debugger server started.\n");
    psvDebugScreenPrintf("Ctrl-C and clean reconnect are enabled.\n");
    for(int i = 0;; i++)
    {
        if(trigger_fault)
            *(volatile unsigned int*)0 = 0x55464442;
        test_value = step_target(i);
        thumb_step_pop_fixture();
        thumb_step_mov_fixture();
        thumb_step_tbb_fixture();
        thumb_step_tbh_fixture();
        thumb_step_ldm_fixture();
        thumb_step_it_fixture();
        thumb_step_ldmdb_fixture();
        arm_step_mov_fixture();
        arm_step_ldm_fixture();
        if((i % 10) == 0)
            psvDebugScreenPrintf("alive: i=%d value=%d\n", i, test_value);
        usleep(100000);
    }
}
