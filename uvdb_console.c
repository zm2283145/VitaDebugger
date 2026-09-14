#include <limits.h>
#include <string.h>

#include "uvdb_console.h"

#define UVDB_CONSOLE_SESSION_OPEN UINT32_C(0x80000000)
#define UVDB_CONSOLE_SESSION_GENERATION UINT32_C(0x7fffffff)

struct uvdb_console_counter_state {
    uint32_t queued_records;
    uint32_t queued_bytes;
    uint32_t sessions_opened;
    uint32_t reconnects;
    uint32_t accepted_records;
    uint32_t accepted_bytes;
    uint32_t sent_records;
    uint32_t sent_bytes;
    uint32_t dropped_disconnected_records;
    uint32_t dropped_disconnected_bytes;
    uint32_t dropped_contention_records;
    uint32_t dropped_contention_bytes;
    uint32_t dropped_full_records;
    uint32_t dropped_full_bytes;
    uint32_t dropped_stale_records;
    uint32_t dropped_stale_bytes;
};

static struct uvdb_console_record console_queue[UVDB_CONSOLE_QUEUE_SLOTS];
static uint32_t console_head;
static uint32_t console_tail;
static uint32_t console_count;
static uint32_t console_bytes;
static uint64_t console_next_sequence;
static uint32_t console_session;
static uint32_t console_ever_opened;
static uint32_t console_queue_lock;
static uint32_t console_lifecycle_lock;
static struct uvdb_console_counter_state console_stats;
#ifdef UVDB_CONSOLE_TESTING
static uint32_t console_test_pause_capture;
static uint32_t console_test_capture_paused;
#endif

static int console_try_lock(uint32_t* lock)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(lock, &expected, 1, 1,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void console_unlock(uint32_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static int console_generation_valid(uint32_t generation)
{
    return generation != 0 &&
           !(generation & ~UVDB_CONSOLE_SESSION_GENERATION);
}

static uint32_t console_record_count(size_t size)
{
    size_t count = size / UVDB_CONSOLE_RECORD_MAX;
    if(size % UVDB_CONSOLE_RECORD_MAX)
        count++;
    return (uint32_t)count;
}

static void console_add(uint32_t* counter, size_t amount)
{
    __atomic_add_fetch(counter, (uint32_t)amount, __ATOMIC_RELAXED);
}

static void console_note_drop(uint32_t* records, uint32_t* bytes, size_t size)
{
    if(!size)
        return;
    console_add(records, console_record_count(size));
    console_add(bytes, size);
}

static void console_publish_depth(void)
{
    __atomic_store_n(&console_stats.queued_records, console_count,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&console_stats.queued_bytes, console_bytes,
                     __ATOMIC_RELAXED);
}

static void console_drop_head_as_stale(void)
{
    struct uvdb_console_record* record = &console_queue[console_head];
    console_add(&console_stats.dropped_stale_records, 1);
    console_add(&console_stats.dropped_stale_bytes, record->size);
    console_bytes -= (uint32_t)record->size;
    console_head = (console_head + 1u) % UVDB_CONSOLE_QUEUE_SLOTS;
    console_count--;
}

static void console_purge_locked(void)
{
    while(console_count)
        console_drop_head_as_stale();
    console_head = 0;
    console_tail = 0;
    console_publish_depth();
}

static void console_purge_other_generations_locked(uint32_t generation)
{
    while(console_count &&
          console_queue[console_head].generation != generation)
        console_drop_head_as_stale();
    console_publish_depth();
}

void uvdb_console_reset(void)
{
    memset(console_queue, 0, sizeof(console_queue));
    memset(&console_stats, 0, sizeof(console_stats));
    console_head = 0;
    console_tail = 0;
    console_count = 0;
    console_bytes = 0;
    console_next_sequence = 0;
    console_ever_opened = 0;
    __atomic_store_n(&console_session, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&console_queue_lock, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&console_lifecycle_lock, 0, __ATOMIC_RELEASE);
#ifdef UVDB_CONSOLE_TESTING
    __atomic_store_n(&console_test_pause_capture, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&console_test_capture_paused, 0, __ATOMIC_RELEASE);
#endif
}

uint32_t uvdb_console_session_open(void)
{
    if(!console_try_lock(&console_lifecycle_lock))
        return 0;

    uint32_t old_session =
        __atomic_load_n(&console_session, __ATOMIC_ACQUIRE);
    uint32_t old_generation =
        old_session & UVDB_CONSOLE_SESSION_GENERATION;
    __atomic_store_n(&console_session, old_generation, __ATOMIC_RELEASE);

    if(console_try_lock(&console_queue_lock))
    {
        console_purge_locked();
        console_unlock(&console_queue_lock);
    }

    uint32_t generation = old_generation + 1u;
    generation &= UVDB_CONSOLE_SESSION_GENERATION;
    if(!generation)
        generation = 1u;

    console_add(&console_stats.sessions_opened, 1);
    if(console_ever_opened)
        console_add(&console_stats.reconnects, 1);
    else
        console_ever_opened = 1;
    __atomic_store_n(&console_session,
                     generation | UVDB_CONSOLE_SESSION_OPEN,
                     __ATOMIC_RELEASE);
    console_unlock(&console_lifecycle_lock);
    return generation;
}

int uvdb_console_session_close(uint32_t generation)
{
    if(!console_generation_valid(generation))
        return UVDB_CONSOLE_ERROR;

    uint32_t expected = generation | UVDB_CONSOLE_SESSION_OPEN;
    for(;;)
    {
        uint32_t session =
            __atomic_load_n(&console_session, __ATOMIC_ACQUIRE);
        if(session == generation)
            break;
        if(session != expected)
            return UVDB_CONSOLE_STALE;
        if(__atomic_compare_exchange_n(&console_session, &session,
                                       generation, 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE))
            break;
    }

    if(!console_try_lock(&console_lifecycle_lock))
        return UVDB_CONSOLE_BUSY;
    if(!console_try_lock(&console_queue_lock))
    {
        console_unlock(&console_lifecycle_lock);
        return UVDB_CONSOLE_BUSY;
    }
    console_purge_locked();
    console_unlock(&console_queue_lock);
    console_unlock(&console_lifecycle_lock);
    return UVDB_CONSOLE_READY;
}

void uvdb_console_session_close_active_gate(void)
{
    uint32_t session =
        __atomic_load_n(&console_session, __ATOMIC_ACQUIRE);
    while(session & UVDB_CONSOLE_SESSION_OPEN)
    {
        uint32_t closed = session & UVDB_CONSOLE_SESSION_GENERATION;
        if(__atomic_compare_exchange_n(&console_session, &session, closed, 0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE))
            return;
    }
}

size_t uvdb_console_capture(const void* data, size_t size)
{
    if(!size)
        return 0;
    if(!data)
        return 0;

    uint32_t session =
        __atomic_load_n(&console_session, __ATOMIC_ACQUIRE);
    if(!(session & UVDB_CONSOLE_SESSION_OPEN))
    {
        console_note_drop(&console_stats.dropped_disconnected_records,
                          &console_stats.dropped_disconnected_bytes, size);
        return 0;
    }
    if(!console_try_lock(&console_queue_lock))
    {
        console_note_drop(&console_stats.dropped_contention_records,
                          &console_stats.dropped_contention_bytes, size);
        return 0;
    }

#ifdef UVDB_CONSOLE_TESTING
    if(__atomic_load_n(&console_test_pause_capture, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&console_test_capture_paused, 1, __ATOMIC_RELEASE);
        while(__atomic_load_n(&console_test_pause_capture, __ATOMIC_ACQUIRE))
        {
        }
        __atomic_store_n(&console_test_capture_paused, 0, __ATOMIC_RELEASE);
    }
#endif

    if(__atomic_load_n(&console_session, __ATOMIC_ACQUIRE) != session)
    {
        console_unlock(&console_queue_lock);
        console_note_drop(&console_stats.dropped_stale_records,
                          &console_stats.dropped_stale_bytes, size);
        return 0;
    }

    const unsigned char* cursor = data;
    size_t remaining = size;
    size_t accepted = 0;
    uint32_t records = 0;
    uint32_t generation = session & UVDB_CONSOLE_SESSION_GENERATION;
    console_purge_other_generations_locked(generation);
    while(remaining && console_count < UVDB_CONSOLE_QUEUE_SLOTS)
    {
        size_t chunk = remaining;
        if(chunk > UVDB_CONSOLE_RECORD_MAX)
            chunk = UVDB_CONSOLE_RECORD_MAX;

        struct uvdb_console_record* record = &console_queue[console_tail];
        record->generation = generation;
        console_next_sequence++;
        if(!console_next_sequence)
            console_next_sequence++;
        record->sequence = console_next_sequence;
        record->size = chunk;
        memcpy(record->data, cursor, chunk);

        console_tail = (console_tail + 1u) % UVDB_CONSOLE_QUEUE_SLOTS;
        console_count++;
        console_bytes += (uint32_t)chunk;
        cursor += chunk;
        remaining -= chunk;
        accepted += chunk;
        records++;
    }
    console_publish_depth();
    console_unlock(&console_queue_lock);

    if(records)
    {
        console_add(&console_stats.accepted_records, records);
        console_add(&console_stats.accepted_bytes, accepted);
    }
    if(remaining)
        console_note_drop(&console_stats.dropped_full_records,
                          &console_stats.dropped_full_bytes, remaining);
    return accepted;
}

int uvdb_console_peek(uint32_t generation,
                      struct uvdb_console_record* record)
{
    if(!record || !console_generation_valid(generation))
        return UVDB_CONSOLE_ERROR;
    uint32_t expected =
        generation | UVDB_CONSOLE_SESSION_OPEN;
    if(__atomic_load_n(&console_session, __ATOMIC_ACQUIRE) != expected)
        return UVDB_CONSOLE_STALE;
    if(!console_try_lock(&console_queue_lock))
        return UVDB_CONSOLE_BUSY;
    if(__atomic_load_n(&console_session, __ATOMIC_ACQUIRE) != expected)
    {
        console_unlock(&console_queue_lock);
        return UVDB_CONSOLE_STALE;
    }

    console_purge_other_generations_locked(generation);
    if(!console_count)
    {
        console_unlock(&console_queue_lock);
        return UVDB_CONSOLE_EMPTY;
    }
    *record = console_queue[console_head];
    console_unlock(&console_queue_lock);
    return UVDB_CONSOLE_READY;
}

int uvdb_console_commit_sent(uint32_t generation, uint64_t sequence)
{
    if(!console_generation_valid(generation) || !sequence)
        return UVDB_CONSOLE_ERROR;
    uint32_t expected =
        generation | UVDB_CONSOLE_SESSION_OPEN;
    if(__atomic_load_n(&console_session, __ATOMIC_ACQUIRE) != expected)
        return UVDB_CONSOLE_STALE;
    if(!console_try_lock(&console_queue_lock))
        return UVDB_CONSOLE_BUSY;
    if(__atomic_load_n(&console_session, __ATOMIC_ACQUIRE) != expected)
    {
        console_unlock(&console_queue_lock);
        return UVDB_CONSOLE_STALE;
    }

    console_purge_other_generations_locked(generation);
    if(!console_count || console_queue[console_head].sequence != sequence)
    {
        console_unlock(&console_queue_lock);
        return UVDB_CONSOLE_STALE;
    }

    size_t size = console_queue[console_head].size;
    console_bytes -= (uint32_t)size;
    console_head = (console_head + 1u) % UVDB_CONSOLE_QUEUE_SLOTS;
    console_count--;
    console_publish_depth();
    console_unlock(&console_queue_lock);

    console_add(&console_stats.sent_records, 1);
    console_add(&console_stats.sent_bytes, size);
    return UVDB_CONSOLE_READY;
}

int uvdb_console_get_stats(struct uvdb_console_stats* stats)
{
    if(!stats)
        return -1;

#define LOAD_STAT(name) \
    stats->name = __atomic_load_n(&console_stats.name, __ATOMIC_RELAXED)
    LOAD_STAT(queued_records);
    LOAD_STAT(queued_bytes);
    LOAD_STAT(sessions_opened);
    LOAD_STAT(reconnects);
    LOAD_STAT(accepted_records);
    LOAD_STAT(accepted_bytes);
    LOAD_STAT(sent_records);
    LOAD_STAT(sent_bytes);
    LOAD_STAT(dropped_disconnected_records);
    LOAD_STAT(dropped_disconnected_bytes);
    LOAD_STAT(dropped_contention_records);
    LOAD_STAT(dropped_contention_bytes);
    LOAD_STAT(dropped_full_records);
    LOAD_STAT(dropped_full_bytes);
    LOAD_STAT(dropped_stale_records);
    LOAD_STAT(dropped_stale_bytes);
#undef LOAD_STAT

    uint32_t session =
        __atomic_load_n(&console_session, __ATOMIC_ACQUIRE);
    stats->session_generation =
        session & UVDB_CONSOLE_SESSION_GENERATION;
    stats->session_open =
        (session & UVDB_CONSOLE_SESSION_OPEN) != 0;
    return 0;
}

#ifdef UVDB_CONSOLE_TESTING
int uvdb_console_test_lock_queue(void)
{
    return console_try_lock(&console_queue_lock);
}

void uvdb_console_test_unlock_queue(void)
{
    console_unlock(&console_queue_lock);
}

int uvdb_console_test_lock_lifecycle(void)
{
    return console_try_lock(&console_lifecycle_lock);
}

void uvdb_console_test_unlock_lifecycle(void)
{
    console_unlock(&console_lifecycle_lock);
}

void uvdb_console_test_set_generation(uint32_t generation)
{
    __atomic_store_n(&console_session,
                     generation & UVDB_CONSOLE_SESSION_GENERATION,
                     __ATOMIC_RELEASE);
}

void uvdb_console_test_pause_capture_after_lock(int pause)
{
    __atomic_store_n(&console_test_pause_capture, pause != 0,
                     __ATOMIC_RELEASE);
}

int uvdb_console_test_capture_is_paused(void)
{
    return __atomic_load_n(&console_test_capture_paused,
                           __ATOMIC_ACQUIRE) != 0;
}
#endif
