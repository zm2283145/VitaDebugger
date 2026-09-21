#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_protocol_gate.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int wait_flag(volatile uint32_t* flag)
{
    for(unsigned int attempt = 0; attempt < 1000000u; ++attempt)
    {
        if(__atomic_load_n(flag, __ATOMIC_ACQUIRE))
            return 1;
        sched_yield();
    }
    return 0;
}

struct protocol_fixture {
    struct uvdb_protocol_gate gate;
    volatile uint32_t owner_ready;
    volatile uint32_t close_published;
    volatile uint32_t owner_touched_after_close;
    volatile uint32_t release_owner;
    volatile uint32_t fake_storage_freed;
    volatile uint32_t unsafe_touch;
    unsigned char fake_packet_buffer[64];
    int acquire_result;
    int release_result;
};

static void* protocol_owner_main(void* opaque)
{
    struct protocol_fixture* fixture = opaque;
    fixture->acquire_result = uvdb_protocol_gate_try_acquire(
        &fixture->gate, UINT32_C(0x1111));
    __atomic_store_n(&fixture->owner_ready, 1u, __ATOMIC_RELEASE);
    if(fixture->acquire_result != UVDB_PROTOCOL_GATE_ACQUIRED)
        return NULL;

    (void)wait_flag(&fixture->close_published);
    if(__atomic_load_n(&fixture->fake_storage_freed, __ATOMIC_ACQUIRE))
        __atomic_store_n(&fixture->unsafe_touch, 1u, __ATOMIC_RELEASE);
    else
        fixture->fake_packet_buffer[0] = 0xa5;
    __atomic_store_n(&fixture->owner_touched_after_close, 1u,
                     __ATOMIC_RELEASE);
    (void)wait_flag(&fixture->release_owner);
    fixture->release_result = uvdb_protocol_gate_release(
        &fixture->gate, UINT32_C(0x1111));
    return NULL;
}

static void test_close_vs_blocking_owner(void)
{
    struct protocol_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    uvdb_protocol_gate_init(&fixture.gate);

    pthread_t owner_thread;
    check(pthread_create(&owner_thread, NULL, protocol_owner_main,
                         &fixture) == 0,
          "create fake blocking protocol owner");
    check(wait_flag(&fixture.owner_ready),
          "fake protocol owner acquired whole session");
    check(fixture.acquire_result == UVDB_PROTOCOL_GATE_ACQUIRED,
          "first packet path acquires gate");
    check(uvdb_protocol_gate_try_acquire(
              &fixture.gate, UINT32_C(0x2222)) ==
              UVDB_PROTOCOL_GATE_BUSY,
          "simultaneous remote syscall cannot share packet buffers");
    check(uvdb_protocol_gate_release(
              &fixture.gate, UINT32_C(0x2222)) < 0,
          "non-owner cannot release packet lifetime");

    uvdb_protocol_gate_close(&fixture.gate);
    __atomic_store_n(&fixture.close_published, 1u, __ATOMIC_RELEASE);
    check(wait_flag(&fixture.owner_touched_after_close),
          "in-flight owner completed fake buffer access after close");
    check(!uvdb_protocol_gate_is_idle(&fixture.gate) &&
              uvdb_protocol_gate_is_closing(&fixture.gate),
          "close remains visible while buffer owner drains");
    check(!fixture.unsafe_touch && fixture.fake_packet_buffer[0] == 0xa5,
          "teardown retained fake buffer across dropped-lock operation");
    check(uvdb_protocol_gate_try_acquire(
              &fixture.gate, UINT32_C(0x3333)) ==
              UVDB_PROTOCOL_GATE_CLOSED,
          "close blocks new exception or syscall protocol owner");
    check(uvdb_protocol_gate_reopen(&fixture.gate) < 0,
          "gate cannot reopen before prior owner drains");

    __atomic_store_n(&fixture.release_owner, 1u, __ATOMIC_RELEASE);
    check(pthread_join(owner_thread, NULL) == 0,
          "join fake protocol owner");
    check(fixture.release_result == 0 &&
              uvdb_protocol_gate_is_idle(&fixture.gate),
          "exact owner publishes protocol quiescence");
    __atomic_store_n(&fixture.fake_storage_freed, 1u, __ATOMIC_RELEASE);
    check(!fixture.unsafe_touch,
          "fake storage freed only after whole-session quiescence");
    check(uvdb_protocol_gate_reopen(&fixture.gate) == 0 &&
              uvdb_protocol_gate_try_acquire(
                  &fixture.gate, UINT32_C(0x4444)) ==
                  UVDB_PROTOCOL_GATE_ACQUIRED &&
              uvdb_protocol_gate_release(
                  &fixture.gate, UINT32_C(0x4444)) == 0,
          "nonterminal stop/start can reopen an idle protocol gate");
}

static void test_invalid_inputs(void)
{
    struct uvdb_protocol_gate gate;
    uvdb_protocol_gate_init(&gate);
    check(uvdb_protocol_gate_try_acquire(&gate, 0) ==
              UVDB_PROTOCOL_GATE_INVALID,
          "zero owner token rejected");
    check(uvdb_protocol_gate_try_acquire(
              &gate, UINT32_C(0x80000000)) ==
              UVDB_PROTOCOL_GATE_INVALID,
          "reserved closing bit cannot be used as an owner token");
    check(uvdb_protocol_gate_try_acquire(NULL, 1) ==
              UVDB_PROTOCOL_GATE_INVALID &&
              uvdb_protocol_gate_release(NULL, 1) < 0 &&
              !uvdb_protocol_gate_is_idle(NULL),
          "NULL protocol gate operations rejected");
}

int main(void)
{
    test_close_vs_blocking_owner();
    test_invalid_inputs();
    if(failures)
        return 1;
    puts("PASS: whole-protocol ownership and concurrent close lifetime");
    return 0;
}
