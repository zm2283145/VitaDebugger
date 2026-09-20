#include "uvdb_exception_guard.h"

#include <string.h>

#define UVDB_EXCEPTION_GUARD_CLOSED_BIT UINT32_C(0x80000000)
#define UVDB_EXCEPTION_GUARD_ACTIVE_MASK UINT32_C(0x7fffffff)

void uvdb_exception_guard_init(struct uvdb_exception_guard* guard)
{
    if(guard)
        memset(guard, 0, sizeof(*guard));
}

int uvdb_exception_guard_enter(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type)
{
    if(!guard || exception_type >= UVDB_EXCEPTION_GUARD_TYPE_COUNT)
        return UVDB_EXCEPTION_GUARD_INVALID;

    uint32_t lifecycle = __atomic_load_n(&guard->lifecycle,
                                          __ATOMIC_ACQUIRE);
    for(;;)
    {
        if((lifecycle & UVDB_EXCEPTION_GUARD_ACTIVE_MASK) ==
           UVDB_EXCEPTION_GUARD_ACTIVE_MASK)
            return UVDB_EXCEPTION_GUARD_INVALID;
        if(__atomic_compare_exchange_n(&guard->lifecycle, &lifecycle,
                                        lifecycle + 1u, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }

    if(lifecycle & UVDB_EXCEPTION_GUARD_CLOSED_BIT)
    {
        __atomic_add_fetch(&guard->closed_entries, 1u,
                           __ATOMIC_RELAXED);
        return UVDB_EXCEPTION_GUARD_CLOSED;
    }

    uint32_t expected = 0;
    const uint32_t owner = exception_type + 1u;
    if(__atomic_compare_exchange_n(&guard->owner, &expected, owner, 0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
    {
        __atomic_add_fetch(&guard->primary_entries, 1u,
                           __ATOMIC_RELAXED);
        return UVDB_EXCEPTION_GUARD_PRIMARY;
    }
    __atomic_add_fetch(&guard->nested_entries, 1u, __ATOMIC_RELAXED);
    return UVDB_EXCEPTION_GUARD_NESTED;
}

int uvdb_exception_guard_begin_chain(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type)
{
    if(!guard || exception_type >= UVDB_EXCEPTION_GUARD_TYPE_COUNT)
        return -1;
    const uint32_t bit = UINT32_C(1) << exception_type;
    uint32_t mask = __atomic_load_n(
        &guard->chaining_mask, __ATOMIC_ACQUIRE);
    for(;;)
    {
        if(mask & bit)
            return 0;
        if(__atomic_compare_exchange_n(
               &guard->chaining_mask, &mask, mask | bit, 0,
               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    __atomic_add_fetch(&guard->chained_entries, 1u, __ATOMIC_RELAXED);
    return 1;
}

void uvdb_exception_guard_end_chain(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type)
{
    if(guard && exception_type < UVDB_EXCEPTION_GUARD_TYPE_COUNT)
        (void)__atomic_and_fetch(
            &guard->chaining_mask,
            ~(UINT32_C(1) << exception_type),
            __ATOMIC_RELEASE);
}

void uvdb_exception_guard_note_unhandled(
    struct uvdb_exception_guard* guard)
{
    if(guard)
        __atomic_add_fetch(&guard->unhandled_nested_entries, 1u,
                           __ATOMIC_RELAXED);
}

int uvdb_exception_guard_leave(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type,
    int entry_result)
{
    if(!guard || exception_type >= UVDB_EXCEPTION_GUARD_TYPE_COUNT ||
       (entry_result != UVDB_EXCEPTION_GUARD_PRIMARY &&
        entry_result != UVDB_EXCEPTION_GUARD_NESTED &&
        entry_result != UVDB_EXCEPTION_GUARD_CLOSED))
        return -1;
    int result = 0;
    if(entry_result == UVDB_EXCEPTION_GUARD_PRIMARY)
    {
        uint32_t expected = exception_type + 1u;
        if(!__atomic_compare_exchange_n(&guard->owner, &expected, 0u, 0,
                                         __ATOMIC_RELEASE,
                                         __ATOMIC_RELAXED))
            return -1;
    }

    /* Do not clear `chaining_mask` here. A simultaneous nested handler may still
     * be executing its predecessor after the primary handler has returned. */
    uint32_t before = __atomic_fetch_sub(&guard->lifecycle, 1u,
                                         __ATOMIC_RELEASE);
    if(!(before & UVDB_EXCEPTION_GUARD_ACTIVE_MASK))
    {
        (void)__atomic_add_fetch(&guard->lifecycle, 1u,
                                 __ATOMIC_RELAXED);
        result = -1;
    }
    return result;
}

void uvdb_exception_guard_close(struct uvdb_exception_guard* guard)
{
    if(guard)
        (void)__atomic_or_fetch(&guard->lifecycle,
                                UVDB_EXCEPTION_GUARD_CLOSED_BIT,
                                __ATOMIC_ACQ_REL);
}

int uvdb_exception_guard_reopen(struct uvdb_exception_guard* guard)
{
    if(!guard || !uvdb_exception_guard_is_idle(guard))
        return -1;
    uint32_t expected = UVDB_EXCEPTION_GUARD_CLOSED_BIT;
    if(__atomic_compare_exchange_n(&guard->lifecycle, &expected, 0u, 0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE) || expected == 0u)
        return 0;
    return -1;
}

int uvdb_exception_guard_is_idle(const struct uvdb_exception_guard* guard)
{
    if(!guard)
        return 0;
    const uint32_t lifecycle = __atomic_load_n(&guard->lifecycle,
                                                __ATOMIC_ACQUIRE);
    return !(lifecycle & UVDB_EXCEPTION_GUARD_ACTIVE_MASK) &&
           !__atomic_load_n(&guard->owner, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&guard->chaining_mask, __ATOMIC_ACQUIRE);
}

int uvdb_exception_guard_get_stats(
    const struct uvdb_exception_guard* guard,
    struct uvdb_exception_guard_stats* stats)
{
    if(!guard || !stats)
        return -1;
    struct uvdb_exception_guard_stats snapshot = {
        .primary_entries = __atomic_load_n(&guard->primary_entries,
                                            __ATOMIC_RELAXED),
        .nested_entries = __atomic_load_n(&guard->nested_entries,
                                           __ATOMIC_RELAXED),
        .chained_entries = __atomic_load_n(&guard->chained_entries,
                                            __ATOMIC_RELAXED),
        .unhandled_nested_entries =
            __atomic_load_n(&guard->unhandled_nested_entries,
                            __ATOMIC_RELAXED),
        .closed_entries =
            __atomic_load_n(&guard->closed_entries,
                            __ATOMIC_RELAXED),
        .active_handlers =
            __atomic_load_n(&guard->lifecycle, __ATOMIC_ACQUIRE) &
                UVDB_EXCEPTION_GUARD_ACTIVE_MASK,
        .closing =
            !!(__atomic_load_n(&guard->lifecycle, __ATOMIC_ACQUIRE) &
               UVDB_EXCEPTION_GUARD_CLOSED_BIT),
    };
    *stats = snapshot;
    return 0;
}
