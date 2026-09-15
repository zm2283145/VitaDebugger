#include "uvdb_protocol_gate.h"

#include <string.h>

void uvdb_protocol_gate_init(struct uvdb_protocol_gate* gate)
{
    if(gate)
        memset(gate, 0, sizeof(*gate));
}

int uvdb_protocol_gate_try_acquire(
    struct uvdb_protocol_gate* gate,
    uint32_t owner)
{
    if(!gate || !owner)
        return UVDB_PROTOCOL_GATE_INVALID;
    if(__atomic_load_n(&gate->closing, __ATOMIC_ACQUIRE))
        return UVDB_PROTOCOL_GATE_CLOSED;

    uint32_t expected = 0;
    if(!__atomic_compare_exchange_n(&gate->owner, &expected, owner, 0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return UVDB_PROTOCOL_GATE_BUSY;

    /* close may have linearized between the first sample and our claim. In
     * that ordering the new owner must retire without touching shared data. */
    if(__atomic_load_n(&gate->closing, __ATOMIC_ACQUIRE))
    {
        expected = owner;
        (void)__atomic_compare_exchange_n(&gate->owner, &expected, 0u, 0,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED);
        return UVDB_PROTOCOL_GATE_CLOSED;
    }
    return UVDB_PROTOCOL_GATE_ACQUIRED;
}

int uvdb_protocol_gate_release(
    struct uvdb_protocol_gate* gate,
    uint32_t owner)
{
    if(!gate || !owner)
        return -1;
    uint32_t expected = owner;
    return __atomic_compare_exchange_n(&gate->owner, &expected, 0u, 0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)
               ? 0 : -1;
}

void uvdb_protocol_gate_close(struct uvdb_protocol_gate* gate)
{
    if(gate)
        __atomic_store_n(&gate->closing, 1u, __ATOMIC_RELEASE);
}

int uvdb_protocol_gate_reopen(struct uvdb_protocol_gate* gate)
{
    if(!gate || __atomic_load_n(&gate->owner, __ATOMIC_ACQUIRE))
        return -1;
    __atomic_store_n(&gate->closing, 0u, __ATOMIC_RELEASE);
    /* Lifecycle callers serialize close/reopen. Recheck protects accidental
     * misuse from publishing an open gate over an outstanding owner. */
    if(__atomic_load_n(&gate->owner, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&gate->closing, 1u, __ATOMIC_RELEASE);
        return -1;
    }
    return 0;
}

int uvdb_protocol_gate_is_idle(const struct uvdb_protocol_gate* gate)
{
    return gate && !__atomic_load_n(&gate->owner, __ATOMIC_ACQUIRE);
}

int uvdb_protocol_gate_is_closing(const struct uvdb_protocol_gate* gate)
{
    return gate && __atomic_load_n(&gate->closing, __ATOMIC_ACQUIRE);
}
