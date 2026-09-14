#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Internal stdout/stderr capture queue.  This file deliberately has no Vita
 * dependencies so its concurrency and reconnect behavior can be host tested.
 * Exactly one debugger transport owner must serialize session_open(),
 * session_close(), peek(), and commit_sent().  It must finish or abandon an
 * in-flight peek before a lifecycle change or ownership handoff.  Capture and
 * statistics calls may come from other threads.
 */
#define UVDB_CONSOLE_QUEUE_SLOTS 64u
#define UVDB_CONSOLE_RECORD_MAX 128u

enum uvdb_console_result {
    UVDB_CONSOLE_ERROR = -1,
    UVDB_CONSOLE_EMPTY = 0,
    UVDB_CONSOLE_READY = 1,
    UVDB_CONSOLE_BUSY = 2,
    UVDB_CONSOLE_STALE = 3,
};

struct uvdb_console_record {
    uint32_t generation;
    uint64_t sequence;
    size_t size;
    unsigned char data[UVDB_CONSOLE_RECORD_MAX];
};

struct uvdb_console_stats {
    /* Current queue/session state. */
    uint32_t queued_records;
    uint32_t queued_bytes;
    uint32_t session_generation;
    uint32_t session_open;

    /* Cumulative counters since uvdb_console_reset(). */
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

/*
 * Reset all queue and statistics state.  The caller must ensure that no other
 * console API operation, including a statistics read, is in progress.
 */
void uvdb_console_reset(void);

/*
 * Start a new output session and return its nonzero generation token.  Returns
 * zero rather than waiting if its one-attempt lifecycle acquisition fails;
 * this can be transient even when no competing lifecycle caller is active.
 * The serialized transport owner must retry later.  Records left by an earlier
 * session are purged immediately when the queue is available, or lazily by the
 * next producer/consumer.  Lifecycle calls never wait for a producer that may
 * have been suspended while holding the bounded-copy lock.
 */
uint32_t uvdb_console_session_open(void);

/*
 * Close only the session identified by generation.  Returns READY when that
 * generation is closed and its queue purged (including an idempotent repeat),
 * STALE for a token superseded by another session, BUSY when lifecycle/queue
 * cleanup could not finish, or ERROR for a malformed token.  Once a matching
 * call begins, its capture gate is closed before BUSY can be returned; retrying
 * the token is needed only for eager queue cleanup.
 */
int uvdb_console_session_close(uint32_t generation);

/*
 * Atomically close whichever capture generation is currently open, without
 * waiting or purging. This is the cross-thread emergency gate used to begin
 * debugger-server shutdown; the serialized transport owner later performs the
 * generation-specific close and cleanup.
 */
void uvdb_console_session_close_active_gate(void);

/*
 * Try to capture size bytes without waiting.  A call takes the queue lock at
 * most once, splits accepted data into fixed-size records, and returns the
 * exact byte count accepted.  Any remainder is accounted as disconnected,
 * contention, stale-session, or full-queue loss.  NULL is valid only for a
 * zero-size call.  Queue acquisition makes one weak compare/exchange attempt;
 * accounting atomics are lock-free on the Vita target but not formally
 * wait-free.
 */
size_t uvdb_console_capture(const void* data, size_t size);

/*
 * Copy (but do not remove) the current head record.  A successful network send
 * must be followed by uvdb_console_commit_sent() with the returned generation
 * and sequence.  Both calls are nonblocking and may return BUSY.  STALE means
 * the supplied generation or record token no longer identifies the live head.
 * These calls are single-consumer operations under the ownership rule above.
 */
int uvdb_console_peek(uint32_t generation,
                      struct uvdb_console_record* record);
int uvdb_console_commit_sent(uint32_t generation, uint64_t sequence);

/* Atomic, individually coherent snapshot; cumulative 32-bit counters wrap. */
int uvdb_console_get_stats(struct uvdb_console_stats* stats);

#ifdef UVDB_CONSOLE_TESTING
/* Deterministically force the producer contention path in native tests. */
int uvdb_console_test_lock_queue(void);
void uvdb_console_test_unlock_queue(void);
int uvdb_console_test_lock_lifecycle(void);
void uvdb_console_test_unlock_lifecycle(void);
void uvdb_console_test_set_generation(uint32_t generation);
void uvdb_console_test_pause_capture_after_lock(int pause);
int uvdb_console_test_capture_is_paused(void);
#endif
