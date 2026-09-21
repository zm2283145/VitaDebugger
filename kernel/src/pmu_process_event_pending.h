#pragma once

#include <stdint.h>

#define VD_PMU_PROCESS_EVENT_EMPTY UINT32_C(0)
#define VD_PMU_PROCESS_EVENT_AMBIGUOUS UINT32_MAX

static inline uint32_t vdPmuProcessEventEncode(int32_t pid)
{
    if(pid < 0 || (uint32_t)pid >= UINT32_MAX - 1u)
        return VD_PMU_PROCESS_EVENT_AMBIGUOUS;
    return (uint32_t)pid + 1u;
}

static inline int vdPmuProcessEventDecode(
    uint32_t state, int32_t* pid)
{
    if(!pid || state == VD_PMU_PROCESS_EVENT_EMPTY ||
       state == VD_PMU_PROCESS_EVENT_AMBIGUOUS)
        return 0;
    *pid = (int32_t)(state - 1u);
    return 1;
}

/* Coalesce repeats for one PID and monotonically escalate any disagreement.
 * The callback performs at most one compare-exchange. A race may conservatively
 * produce ambiguity, but it can never lose an event or block process teardown. */
static inline uint32_t vdPmuProcessEventPublish(
    volatile uint32_t* pending, int32_t pid)
{
    if(!pending)
        return VD_PMU_PROCESS_EVENT_AMBIGUOUS;
    const uint32_t encoded = vdPmuProcessEventEncode(pid);
    uint32_t observed =
        __atomic_load_n(pending, __ATOMIC_ACQUIRE);
    if(observed == VD_PMU_PROCESS_EVENT_AMBIGUOUS ||
       observed == encoded)
        return observed;
    if(observed == VD_PMU_PROCESS_EVENT_EMPTY)
    {
        if(__atomic_compare_exchange_n(
               pending, &observed, encoded, 1,
               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return encoded;
        if(observed == encoded ||
           observed == VD_PMU_PROCESS_EVENT_AMBIGUOUS)
            return observed;
    }
    __atomic_store_n(
        pending, VD_PMU_PROCESS_EVENT_AMBIGUOUS, __ATOMIC_RELEASE);
    return VD_PMU_PROCESS_EVENT_AMBIGUOUS;
}

static inline uint32_t vdPmuProcessEventTake(
    volatile uint32_t* pending)
{
    if(!pending)
        return VD_PMU_PROCESS_EVENT_AMBIGUOUS;
    return __atomic_exchange_n(
        pending, VD_PMU_PROCESS_EVENT_EMPTY, __ATOMIC_ACQ_REL);
}
