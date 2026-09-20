#include "uvdb_protocol_gate.h"

#include <string.h>

#define UVDB_PROTOCOL_GATE_CLOSING UINT32_C(0x80000000)
#define UVDB_PROTOCOL_GATE_OWNER UINT32_C(0x7fffffff)

void uvdb_protocol_gate_init(struct uvdb_protocol_gate* gate)
{
    if(gate)
        memset(gate, 0, sizeof(*gate));
}

int uvdb_protocol_gate_try_acquire(
    struct uvdb_protocol_gate* gate,
    uint32_t owner)
{
    if(!gate || !owner || (owner & UVDB_PROTOCOL_GATE_CLOSING))
        return UVDB_PROTOCOL_GATE_INVALID;

    uint32_t expected = 0;
    if(__atomic_compare_exchange_n(
           &gate->state, &expected, owner, 0,
           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return UVDB_PROTOCOL_GATE_ACQUIRED;
    return expected & UVDB_PROTOCOL_GATE_CLOSING
               ? UVDB_PROTOCOL_GATE_CLOSED
               : UVDB_PROTOCOL_GATE_BUSY;
}

int uvdb_protocol_gate_release(
    struct uvdb_protocol_gate* gate,
    uint32_t owner)
{
    if(!gate || !owner || (owner & UVDB_PROTOCOL_GATE_CLOSING))
        return -1;
    uint32_t observed = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    for(;;)
    {
        if((observed & UVDB_PROTOCOL_GATE_OWNER) != owner)
            return -1;
        uint32_t desired = observed & UVDB_PROTOCOL_GATE_CLOSING;
        if(__atomic_compare_exchange_n(
               &gate->state, &observed, desired, 0,
               __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            return 0;
    }
}

void uvdb_protocol_gate_close(struct uvdb_protocol_gate* gate)
{
    if(!gate)
        return;
    __atomic_fetch_or(
        &gate->state, UVDB_PROTOCOL_GATE_CLOSING, __ATOMIC_ACQ_REL);
}

int uvdb_protocol_gate_reopen(struct uvdb_protocol_gate* gate)
{
    if(!gate)
        return -1;
    uint32_t expected = 0;
    if(__atomic_compare_exchange_n(
           &gate->state, &expected, 0u, 0,
           __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        return 0;
    if(expected != UVDB_PROTOCOL_GATE_CLOSING)
        return -1;
    return __atomic_compare_exchange_n(
               &gate->state, &expected, 0u, 0,
               __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)
               ? 0 : -1;
}

int uvdb_protocol_gate_is_idle(const struct uvdb_protocol_gate* gate)
{
    return gate &&
           !(__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) &
             UVDB_PROTOCOL_GATE_OWNER);
}

int uvdb_protocol_gate_is_closing(const struct uvdb_protocol_gate* gate)
{
    return gate &&
           (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) &
            UVDB_PROTOCOL_GATE_CLOSING);
}
