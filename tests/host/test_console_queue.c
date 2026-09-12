#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_console.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static struct uvdb_console_stats get_stats(void)
{
    struct uvdb_console_stats stats;
    memset(&stats, 0xcc, sizeof(stats));
    check(uvdb_console_get_stats(&stats) == 0, "read queue statistics");
    return stats;
}

struct capture_args {
    const void* data;
    size_t size;
    size_t accepted;
    uint32_t done;
};

struct peek_args {
    uint32_t generation;
    struct uvdb_console_record record;
    int result;
    uint32_t done;
};

static void* capture_main(void* argument)
{
    struct capture_args* args = argument;
    args->accepted = uvdb_console_capture(args->data, args->size);
    __atomic_store_n(&args->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void* peek_main(void* argument)
{
    struct peek_args* args = argument;
    args->result = uvdb_console_peek(args->generation, &args->record);
    __atomic_store_n(&args->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int wait_for_completion(uint32_t* done)
{
    for(unsigned int attempt = 0; attempt < 1000000u; ++attempt)
    {
        if(__atomic_load_n(done, __ATOMIC_ACQUIRE))
            return 1;
        sched_yield();
    }
    return 0;
}

static void test_disconnected_and_session_gate(void)
{
    static const char text[] = "offline";
    uvdb_console_reset();
    check(uvdb_console_get_stats(NULL) < 0, "NULL statistics rejected");
    check(uvdb_console_capture(NULL, 0) == 0, "empty capture is a no-op");
    check(uvdb_console_capture(text, sizeof(text) - 1) == 0,
          "disconnected capture rejected");

    struct uvdb_console_stats stats = get_stats();
    check(stats.dropped_disconnected_records == 1 &&
              stats.dropped_disconnected_bytes == sizeof(text) - 1,
          "disconnected loss counted");
    check(stats.queued_records == 0 && stats.queued_bytes == 0,
          "disconnected capture does not queue");

    uint32_t generation = uvdb_console_session_open();
    check(generation != 0, "session generation is nonzero");
    stats = get_stats();
    check(stats.session_open == 1 &&
              stats.session_generation == generation,
          "session gate opens");
    check(stats.sessions_opened == 1 && stats.reconnects == 0,
          "first session statistics");
    check(uvdb_console_session_close(generation + 1) == UVDB_CONSOLE_STALE,
          "stale close cannot close live session");
    check(get_stats().session_open == 1,
          "stale close leaves session open");
    check(uvdb_console_session_close(generation) == UVDB_CONSOLE_READY,
          "live session closes");
    check(uvdb_console_session_close(generation) == UVDB_CONSOLE_READY,
          "completed session close is idempotent");
    check(get_stats().session_open == 0, "session gate closes");
}

static void test_chunk_boundaries_and_peek_commit(void)
{
    unsigned char input[UVDB_CONSOLE_RECORD_MAX + 2];
    for(size_t i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)i;

    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();
    check(uvdb_console_capture(input, sizeof(input)) == sizeof(input),
          "capture splits across records");
    struct uvdb_console_stats stats = get_stats();
    check(stats.queued_records == 2 && stats.queued_bytes == sizeof(input),
          "split capture depth");
    check(stats.accepted_records == 2 &&
              stats.accepted_bytes == sizeof(input),
          "split capture accepted statistics");

    uint32_t malformed = generation | UINT32_C(0x80000000);
    struct uvdb_console_record invalid;
    check(uvdb_console_peek(malformed, &invalid) == UVDB_CONSOLE_ERROR,
          "generation with reserved session bit is rejected");
    check(uvdb_console_session_close(malformed) == UVDB_CONSOLE_ERROR,
          "malformed close token is rejected");
    stats = get_stats();
    check(stats.session_open == 1 && stats.queued_records == 2,
          "malformed generation cannot purge or close live output");

    struct uvdb_console_record first;
    struct uvdb_console_record repeated;
    check(uvdb_console_peek(generation, &first) == UVDB_CONSOLE_READY,
          "first record ready");
    check(first.generation == generation &&
              first.size == UVDB_CONSOLE_RECORD_MAX &&
              memcmp(first.data, input, first.size) == 0,
          "first record exact contents");
    check(uvdb_console_peek(generation, &repeated) == UVDB_CONSOLE_READY &&
              repeated.sequence == first.sequence &&
              repeated.size == first.size,
          "peek without commit is stable");
    check(get_stats().queued_records == 2,
          "peek without commit preserves depth");
    check(uvdb_console_commit_sent(generation, first.sequence + 1) ==
              UVDB_CONSOLE_STALE,
          "wrong commit token rejected");
    check(get_stats().queued_records == 2,
          "wrong commit preserves queue");
    check(uvdb_console_commit_sent(generation, first.sequence) ==
              UVDB_CONSOLE_READY,
          "first record committed");

    struct uvdb_console_record second;
    check(uvdb_console_peek(generation, &second) == UVDB_CONSOLE_READY,
          "second record ready");
    check(second.size == 2 &&
              memcmp(second.data, input + UVDB_CONSOLE_RECORD_MAX, 2) == 0,
          "second record exact contents");
    check(uvdb_console_commit_sent(generation, second.sequence) ==
              UVDB_CONSOLE_READY,
          "second record committed");
    check(uvdb_console_peek(generation, &second) == UVDB_CONSOLE_EMPTY,
          "queue empty after commits");
    stats = get_stats();
    check(stats.sent_records == 2 && stats.sent_bytes == sizeof(input),
          "commit updates sent statistics");
    check(stats.queued_records == 0 && stats.queued_bytes == 0,
          "commit updates queue depth");
}

static void test_full_queue_and_wraparound(void)
{
    unsigned char record[UVDB_CONSOLE_RECORD_MAX];
    memset(record, 0xa5, sizeof(record));
    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();

    for(uint32_t i = 0; i < UVDB_CONSOLE_QUEUE_SLOTS; ++i)
    {
        record[0] = (unsigned char)i;
        check(uvdb_console_capture(record, sizeof(record)) == sizeof(record),
              "fill queue record");
    }
    static const unsigned char overflow[] = {1, 2, 3, 4, 5, 6, 7};
    check(uvdb_console_capture(overflow, sizeof(overflow)) == 0,
          "full queue rejects new bytes");
    struct uvdb_console_stats stats = get_stats();
    check(stats.queued_records == UVDB_CONSOLE_QUEUE_SLOTS &&
              stats.queued_bytes ==
                  UVDB_CONSOLE_QUEUE_SLOTS * UVDB_CONSOLE_RECORD_MAX,
          "full queue exact capacity");
    check(stats.dropped_full_records == 1 &&
              stats.dropped_full_bytes == sizeof(overflow),
          "full queue loss counted");

    for(uint32_t i = 0; i < UVDB_CONSOLE_QUEUE_SLOTS / 2; ++i)
    {
        struct uvdb_console_record head;
        check(uvdb_console_peek(generation, &head) == UVDB_CONSOLE_READY &&
                  head.data[0] == (unsigned char)i,
              "first half FIFO order");
        check(uvdb_console_commit_sent(generation, head.sequence) ==
                  UVDB_CONSOLE_READY,
              "commit first half");
    }
    for(uint32_t i = 0; i < UVDB_CONSOLE_QUEUE_SLOTS / 2; ++i)
    {
        record[0] = (unsigned char)(UVDB_CONSOLE_QUEUE_SLOTS + i);
        check(uvdb_console_capture(record, sizeof(record)) == sizeof(record),
              "wraparound enqueue");
    }
    for(uint32_t i = UVDB_CONSOLE_QUEUE_SLOTS / 2;
        i < UVDB_CONSOLE_QUEUE_SLOTS + UVDB_CONSOLE_QUEUE_SLOTS / 2;
        ++i)
    {
        struct uvdb_console_record head;
        check(uvdb_console_peek(generation, &head) == UVDB_CONSOLE_READY &&
                  head.data[0] == (unsigned char)i,
              "wrapped FIFO order");
        check(uvdb_console_commit_sent(generation, head.sequence) ==
                  UVDB_CONSOLE_READY,
              "commit wrapped record");
    }
    check(get_stats().queued_records == 0, "wrapped queue drains");
}

static void test_partial_acceptance(void)
{
    unsigned char record[UVDB_CONSOLE_RECORD_MAX];
    unsigned char input[UVDB_CONSOLE_RECORD_MAX + 2];
    memset(record, 0x31, sizeof(record));
    memset(input, 0x42, sizeof(input));
    uvdb_console_reset();
    uvdb_console_session_open();

    for(uint32_t i = 0; i < UVDB_CONSOLE_QUEUE_SLOTS - 1u; ++i)
        check(uvdb_console_capture(record, sizeof(record)) == sizeof(record),
              "reserve one queue slot");
    check(uvdb_console_capture(input, sizeof(input)) ==
              UVDB_CONSOLE_RECORD_MAX,
          "capture accepts only the final available slot");

    struct uvdb_console_stats stats = get_stats();
    check(stats.queued_records == UVDB_CONSOLE_QUEUE_SLOTS &&
              stats.queued_bytes ==
                  UVDB_CONSOLE_QUEUE_SLOTS * UVDB_CONSOLE_RECORD_MAX,
          "partial capture fills exact queue capacity");
    check(stats.dropped_full_records == 1 &&
              stats.dropped_full_bytes == 2,
          "partial capture accounts the exact remainder");
}

static void test_forced_contention(void)
{
    unsigned char input[UVDB_CONSOLE_RECORD_MAX + 1];
    memset(input, 0x5a, sizeof(input));
    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();
    check(uvdb_console_test_lock_queue() == 1,
          "test hook acquires queue lock");

    struct capture_args producer = {
        .data = input,
        .size = sizeof(input),
        .accepted = 0,
        .done = 0,
    };
    struct peek_args consumer = {
        .generation = generation,
        .result = UVDB_CONSOLE_ERROR,
        .done = 0,
    };
    pthread_t producer_thread;
    pthread_t consumer_thread;
    int producer_created =
        pthread_create(&producer_thread, NULL, capture_main, &producer) == 0;
    int consumer_created =
        pthread_create(&consumer_thread, NULL, peek_main, &consumer) == 0;
    check(producer_created, "create contended producer");
    check(consumer_created, "create contended consumer");
    int producer_finished =
        producer_created && wait_for_completion(&producer.done);
    int consumer_finished =
        consumer_created && wait_for_completion(&consumer.done);
    check(producer_finished,
          "contended producer finishes before queue unlock");
    check(consumer_finished,
          "contended consumer finishes before queue unlock");
    uvdb_console_test_unlock_queue();
    if(producer_created)
        check(pthread_join(producer_thread, NULL) == 0,
              "join contended producer");
    if(consumer_created)
        check(pthread_join(consumer_thread, NULL) == 0,
              "join contended consumer");
    if(producer_created)
        check(producer.accepted == 0,
              "contended producer accepts no bytes");
    if(consumer_created)
        check(consumer.result == UVDB_CONSOLE_BUSY,
              "contended consumer reports busy");

    struct uvdb_console_stats stats = get_stats();
    check(stats.dropped_contention_records == 2 &&
              stats.dropped_contention_bytes == sizeof(input),
          "contention loss counted");
    check(stats.queued_records == 0, "contention queues nothing");
}

static void test_reconnect_stale_purge(void)
{
    static const char old_text[] = "old session";
    static const char new_text[] = "new session";
    uvdb_console_reset();
    uint32_t first = uvdb_console_session_open();
    check(uvdb_console_capture(old_text, sizeof(old_text) - 1) ==
              sizeof(old_text) - 1,
          "old session capture");

    uint32_t second = uvdb_console_session_open();
    check(second != first && second != 0, "reconnect advances generation");
    struct uvdb_console_stats stats = get_stats();
    check(stats.sessions_opened == 2 && stats.reconnects == 1,
          "reconnect statistics");
    check(stats.dropped_stale_records == 1 &&
              stats.dropped_stale_bytes == sizeof(old_text) - 1,
          "reconnect purges stale queue");
    check(stats.queued_records == 0 && stats.queued_bytes == 0,
          "new session starts without old output");

    struct uvdb_console_record record;
    check(uvdb_console_peek(first, &record) == UVDB_CONSOLE_STALE,
          "old generation cannot peek");
    check(uvdb_console_capture(new_text, sizeof(new_text) - 1) ==
              sizeof(new_text) - 1,
          "new session capture");
    check(uvdb_console_peek(second, &record) == UVDB_CONSOLE_READY &&
              record.size == sizeof(new_text) - 1 &&
              memcmp(record.data, new_text, record.size) == 0,
          "new session sees only new output");
    check(uvdb_console_commit_sent(first, record.sequence) ==
              UVDB_CONSOLE_STALE,
          "old generation cannot commit new record");
    check(uvdb_console_commit_sent(second, record.sequence) ==
              UVDB_CONSOLE_READY,
          "new generation commits new record");
    check(uvdb_console_session_close(first) == UVDB_CONSOLE_STALE,
          "old close cannot affect reconnect");
    check(uvdb_console_session_close(second) == UVDB_CONSOLE_READY,
          "new session closes");
    check(uvdb_console_capture(new_text, sizeof(new_text) - 1) == 0,
          "post-close capture rejected");
    stats = get_stats();
    check(stats.dropped_disconnected_records == 1 &&
              stats.dropped_disconnected_bytes == sizeof(new_text) - 1,
          "post-close loss counted as disconnected");
}

static void test_generation_wrap(void)
{
    uvdb_console_reset();
    uvdb_console_test_set_generation(UINT32_C(0x7ffffffe));
    uint32_t last = uvdb_console_session_open();
    check(last == UINT32_C(0x7fffffff),
          "generation reaches final nonzero token");
    check(uvdb_console_session_close(last) == UVDB_CONSOLE_READY,
          "final generation closes");
    uint32_t wrapped = uvdb_console_session_open();
    check(wrapped == 1, "generation wraps past reserved zero");
    struct uvdb_console_stats stats = get_stats();
    check(stats.session_generation == 1 && stats.session_open == 1,
          "wrapped generation is live");
    check(stats.sessions_opened == 2 && stats.reconnects == 1,
          "generation wrap preserves lifecycle counters");
}

struct lifecycle_args {
    uint32_t old_generation;
    uint32_t new_generation;
    uint32_t done;
    int close_result;
};

static void* lifecycle_main(void* argument)
{
    struct lifecycle_args* args = argument;
    args->close_result =
        uvdb_console_session_close(args->old_generation);
    args->new_generation = uvdb_console_session_open();
    __atomic_store_n(&args->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int wait_for_paused_capture(void)
{
    for(unsigned int attempt = 0; attempt < 1000000u; ++attempt)
    {
        if(uvdb_console_test_capture_is_paused())
            return 1;
        sched_yield();
    }
    return 0;
}

static int wait_for_lifecycle(void* argument)
{
    struct lifecycle_args* args = argument;
    for(unsigned int attempt = 0; attempt < 1000000u; ++attempt)
    {
        if(__atomic_load_n(&args->done, __ATOMIC_ACQUIRE))
            return 1;
        sched_yield();
    }
    return 0;
}

static void test_close_racing_producer(void)
{
    static const char old_payload[] = "queued before race";
    static const char payload[] = "racing capture";
    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();
    check(uvdb_console_capture(old_payload, sizeof(old_payload) - 1) ==
              sizeof(old_payload) - 1,
          "queue output before lifecycle race");
    uvdb_console_test_pause_capture_after_lock(1);
    struct capture_args producer = {
        .data = payload,
        .size = sizeof(payload) - 1,
        .accepted = 0,
    };
    pthread_t producer_thread;
    int producer_created =
        pthread_create(&producer_thread, NULL, capture_main, &producer) == 0;
    check(producer_created, "create racing producer");
    if(!producer_created)
    {
        uvdb_console_test_pause_capture_after_lock(0);
        return;
    }
    int producer_paused = wait_for_paused_capture();
    check(producer_paused, "producer pauses after acquiring queue lock");
    if(!producer_paused)
    {
        uvdb_console_test_pause_capture_after_lock(0);
        pthread_join(producer_thread, NULL);
        return;
    }

    struct lifecycle_args lifecycle = {
        .old_generation = generation,
        .new_generation = 0,
        .done = 0,
        .close_result = UVDB_CONSOLE_ERROR,
    };
    pthread_t lifecycle_thread;
    int lifecycle_created =
        pthread_create(&lifecycle_thread, NULL, lifecycle_main, &lifecycle) == 0;
    check(lifecycle_created, "create racing lifecycle");
    if(!lifecycle_created)
    {
        uvdb_console_test_pause_capture_after_lock(0);
        pthread_join(producer_thread, NULL);
        return;
    }
    check(wait_for_lifecycle(&lifecycle),
          "close and reopen never wait for a suspended producer");
    uvdb_console_test_pause_capture_after_lock(0);
    check(pthread_join(producer_thread, NULL) == 0,
          "join racing producer");
    check(pthread_join(lifecycle_thread, NULL) == 0,
          "join racing lifecycle");
    check(lifecycle.close_result == UVDB_CONSOLE_BUSY,
          "racing close shuts its gate and defers contended cleanup");
    check(lifecycle.new_generation != 0 &&
              lifecycle.new_generation != generation,
          "racing reopen advances generation");
    check(producer.accepted == 0,
          "producer crossing a lifecycle boundary is rejected");

    struct uvdb_console_stats stats = get_stats();
    check(stats.session_open == 1 &&
              stats.session_generation == lifecycle.new_generation &&
              stats.queued_records == 1,
          "racing reopen leaves old output invalidated for lazy purge");
    check(stats.dropped_stale_records == 1 &&
              stats.dropped_stale_bytes == sizeof(payload) - 1,
          "racing producer loss is counted stale");
    check(uvdb_console_capture(payload, sizeof(payload) - 1) ==
              sizeof(payload) - 1,
          "capture resumes in the new session");
    stats = get_stats();
    check(stats.queued_records == 1 &&
              stats.dropped_stale_records == 2 &&
              stats.dropped_stale_bytes ==
                  sizeof(payload) - 1 + sizeof(old_payload) - 1,
          "new producer purges invalidated output before capture");
    struct uvdb_console_record record;
    check(uvdb_console_peek(lifecycle.new_generation, &record) ==
              UVDB_CONSOLE_READY &&
              record.size == sizeof(payload) - 1 &&
              memcmp(record.data, payload, record.size) == 0,
          "reopened session cannot replay old output");
}

static void test_close_cleanup_retry(void)
{
    static const char queued[] = "queued before close";
    static const char crossing[] = "capture crossing close";
    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();
    check(uvdb_console_capture(queued, sizeof(queued) - 1) ==
              sizeof(queued) - 1,
          "queue output before cleanup retry");

    uvdb_console_test_pause_capture_after_lock(1);
    struct capture_args producer = {
        .data = crossing,
        .size = sizeof(crossing) - 1,
        .accepted = 0,
    };
    pthread_t producer_thread;
    int producer_created =
        pthread_create(&producer_thread, NULL, capture_main, &producer) == 0;
    check(producer_created, "create cleanup-retry producer");
    if(!producer_created)
    {
        uvdb_console_test_pause_capture_after_lock(0);
        return;
    }
    int producer_paused = wait_for_paused_capture();
    check(producer_paused, "cleanup-retry producer holds queue lock");
    if(!producer_paused)
    {
        uvdb_console_test_pause_capture_after_lock(0);
        pthread_join(producer_thread, NULL);
        return;
    }

    check(uvdb_console_session_close(generation) == UVDB_CONSOLE_BUSY,
          "contended close returns a retryable result");
    struct uvdb_console_stats stats = get_stats();
    check(stats.session_open == 0 && stats.queued_records == 1,
          "contended close shuts gate before deferred cleanup");
    uvdb_console_test_pause_capture_after_lock(0);
    check(pthread_join(producer_thread, NULL) == 0,
          "join cleanup-retry producer");
    check(producer.accepted == 0,
          "capture crossing close is rejected");
    check(uvdb_console_session_close(generation) == UVDB_CONSOLE_READY,
          "retry purges an already-closed session");
    stats = get_stats();
    check(stats.session_open == 0 && stats.queued_records == 0 &&
              stats.dropped_stale_records == 2 &&
              stats.dropped_stale_bytes ==
                  sizeof(queued) - 1 + sizeof(crossing) - 1,
          "cleanup retry accounts all stale output");
}

struct producer_args {
    uint32_t id;
    uint32_t* start;
    size_t accepted;
};

static void* producer_main(void* argument)
{
    struct producer_args* args = argument;
    while(!__atomic_load_n(args->start, __ATOMIC_ACQUIRE))
    {
    }
    uint32_t payload = UINT32_C(0xc0de0000) | args->id;
    args->accepted = uvdb_console_capture(&payload, sizeof(payload));
    return NULL;
}

static void test_concurrent_producers(void)
{
    enum { PRODUCERS = 8 };
    pthread_t threads[PRODUCERS];
    struct producer_args args[PRODUCERS];
    uint32_t start = 0;
    unsigned int created = 0;
    unsigned char seen[PRODUCERS];
    memset(seen, 0, sizeof(seen));

    uvdb_console_reset();
    uint32_t generation = uvdb_console_session_open();
    for(uint32_t i = 0; i < PRODUCERS; ++i)
    {
        args[i].id = i;
        args[i].start = &start;
        args[i].accepted = 0;
        if(pthread_create(&threads[i], NULL, producer_main, &args[i]) != 0)
        {
            check(0, "create concurrent producer");
            break;
        }
        created++;
    }
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    for(unsigned int i = 0; i < created; ++i)
        check(pthread_join(threads[i], NULL) == 0,
              "join concurrent producer");

    size_t accepted_bytes = 0;
    for(unsigned int i = 0; i < created; ++i)
        accepted_bytes += args[i].accepted;
    struct uvdb_console_stats stats = get_stats();
    check(stats.accepted_bytes + stats.dropped_contention_bytes ==
              created * sizeof(uint32_t),
          "concurrent bytes accepted or observably dropped");
    check(stats.accepted_bytes == accepted_bytes,
          "concurrent return values match statistics");
    check(stats.dropped_full_bytes == 0,
          "concurrent test does not fill queue");

    for(;;)
    {
        struct uvdb_console_record record;
        int result = uvdb_console_peek(generation, &record);
        if(result == UVDB_CONSOLE_EMPTY)
            break;
        check(result == UVDB_CONSOLE_READY,
              "concurrent record available");
        if(result != UVDB_CONSOLE_READY)
            break;
        uint32_t payload = 0;
        memcpy(&payload, record.data, sizeof(payload));
        uint32_t id = payload & UINT32_C(0xffff);
        check(record.size == sizeof(payload) &&
                  (payload & UINT32_C(0xffff0000)) == UINT32_C(0xc0de0000) &&
                  id < PRODUCERS,
              "concurrent record is intact");
        if(id < PRODUCERS)
        {
            check(!seen[id], "concurrent record is not duplicated");
            seen[id] = 1;
        }
        check(uvdb_console_commit_sent(generation, record.sequence) ==
                  UVDB_CONSOLE_READY,
              "commit concurrent record");
    }
    stats = get_stats();
    check(stats.sent_bytes == accepted_bytes && stats.queued_records == 0,
          "all concurrent accepted bytes drain once");
}

int main(void)
{
    test_disconnected_and_session_gate();
    test_chunk_boundaries_and_peek_commit();
    test_full_queue_and_wraparound();
    test_partial_acceptance();
    test_forced_contention();
    test_reconnect_stale_purge();
    test_generation_wrap();
    test_close_racing_producer();
    test_close_cleanup_retry();
    test_concurrent_producers();
    if(failures)
        return 1;
    puts("PASS: bounded GDB console queue");
    return 0;
}
