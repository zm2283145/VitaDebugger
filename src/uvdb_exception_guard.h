#pragma once

#include <stdint.h>

#define UVDB_EXCEPTION_GUARD_TYPE_COUNT 3u

enum uvdb_exception_guard_result {
    UVDB_EXCEPTION_GUARD_INVALID = -2,
    UVDB_EXCEPTION_GUARD_CLOSED = -1,
    UVDB_EXCEPTION_GUARD_NESTED = 0,
    UVDB_EXCEPTION_GUARD_PRIMARY = 1,
};

struct uvdb_exception_guard_stats {
    uint32_t primary_entries;
    uint32_t nested_entries;
    uint32_t chained_entries;
    uint32_t unhandled_nested_entries;
    uint32_t closed_entries;
    uint32_t active_handlers;
    uint32_t closing;
};

struct uvdb_exception_guard {
    /* Bit 31 closes callback admission; the remaining bits count every
     * admitted handler, including nested handlers while in a predecessor. */
    volatile uint32_t lifecycle;
    volatile uint32_t owner;
    volatile uint32_t chaining;
    volatile uint32_t primary_entries;
    volatile uint32_t nested_entries;
    volatile uint32_t chained_entries;
    volatile uint32_t unhandled_nested_entries;
    volatile uint32_t closed_entries;
};

void uvdb_exception_guard_init(struct uvdb_exception_guard* guard);

/* Admit and count one complete handler callback, then claim the single
 * debugger exception path. A second fault is classified NESTED; a callback
 * dispatched after close is classified CLOSED but remains counted until
 * leave, so teardown can preserve predecessor/state lifetimes. */
int uvdb_exception_guard_enter(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type);

/* Serialize prior-handler invocation. This prevents a fault raised by the
 * chained handler from recursively chaining it forever. */
int uvdb_exception_guard_begin_chain(struct uvdb_exception_guard* guard);
void uvdb_exception_guard_end_chain(struct uvdb_exception_guard* guard);
void uvdb_exception_guard_note_unhandled(
    struct uvdb_exception_guard* guard);

int uvdb_exception_guard_leave(
    struct uvdb_exception_guard* guard,
    uint32_t exception_type,
    int entry_result);
void uvdb_exception_guard_close(struct uvdb_exception_guard* guard);
int uvdb_exception_guard_reopen(struct uvdb_exception_guard* guard);
int uvdb_exception_guard_is_idle(const struct uvdb_exception_guard* guard);
int uvdb_exception_guard_get_stats(
    const struct uvdb_exception_guard* guard,
    struct uvdb_exception_guard_stats* stats);
