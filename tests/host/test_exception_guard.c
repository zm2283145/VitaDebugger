#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_exception_guard.h"

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

static void test_basic_policy(void)
{
    struct uvdb_exception_guard guard;
    uvdb_exception_guard_init(&guard);
    int primary = uvdb_exception_guard_enter(&guard, 0);
    int nested = uvdb_exception_guard_enter(&guard, 1);
    check(primary == UVDB_EXCEPTION_GUARD_PRIMARY,
          "first fault owns debugger exception path");
    check(nested == UVDB_EXCEPTION_GUARD_NESTED,
          "nested fault does not spin on debugger ownership");

    check(uvdb_exception_guard_begin_chain(&guard, 0) == 1,
          "first predecessor invocation owns chain gate");
    check(uvdb_exception_guard_begin_chain(&guard, 1) == 1,
          "distinct predecessor type can chain concurrently");
    check(uvdb_exception_guard_begin_chain(&guard, 0) == 0,
          "fault cycle cannot recursively chain the same predecessor");
    uvdb_exception_guard_end_chain(&guard, 1);
    uvdb_exception_guard_end_chain(&guard, 0);
    uvdb_exception_guard_note_unhandled(&guard);

    check(uvdb_exception_guard_leave(&guard, 0, nested) == 0,
          "nested callback retires its full active lifetime");
    check(uvdb_exception_guard_leave(&guard, 1, primary) < 0,
          "wrong exception type cannot release primary owner");
    check(uvdb_exception_guard_leave(&guard, 0, primary) == 0,
          "primary owner releases exact guard");
    check(uvdb_exception_guard_is_idle(&guard),
          "guard is idle after all complete callbacks retire");

    int second = uvdb_exception_guard_enter(&guard, 2);
    check(second == UVDB_EXCEPTION_GUARD_PRIMARY &&
              uvdb_exception_guard_leave(&guard, 2, second) == 0,
          "guard is reusable after exact release");
    check(uvdb_exception_guard_enter(&guard, 3) ==
              UVDB_EXCEPTION_GUARD_INVALID,
          "invalid exception type rejected");

    struct uvdb_exception_guard_stats stats;
    check(uvdb_exception_guard_get_stats(&guard, &stats) == 0 &&
          stats.primary_entries == 2 && stats.nested_entries == 1 &&
          stats.chained_entries == 2 &&
          stats.unhandled_nested_entries == 1 &&
          stats.closed_entries == 0 && stats.active_handlers == 0 &&
          stats.closing == 0,
          "guard publishes deterministic containment statistics");
}

struct interleave_fixture {
    struct uvdb_exception_guard guard;
    volatile uint32_t primary_ready;
    volatile uint32_t predecessor_active;
    volatile uint32_t release_primary;
    volatile uint32_t primary_done;
    volatile uint32_t release_predecessor;
    volatile uint32_t nested_done;
    int primary_enter;
    int primary_leave;
    int nested_enter;
    int nested_chain;
    int nested_leave;
};

static void* primary_main(void* opaque)
{
    struct interleave_fixture* fixture = opaque;
    fixture->primary_enter =
        uvdb_exception_guard_enter(&fixture->guard, 0);
    __atomic_store_n(&fixture->primary_ready, 1u, __ATOMIC_RELEASE);
    (void)wait_flag(&fixture->release_primary);
    fixture->primary_leave = uvdb_exception_guard_leave(
        &fixture->guard, 0, fixture->primary_enter);
    __atomic_store_n(&fixture->primary_done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

static void* nested_predecessor_main(void* opaque)
{
    struct interleave_fixture* fixture = opaque;
    (void)wait_flag(&fixture->primary_ready);
    fixture->nested_enter =
        uvdb_exception_guard_enter(&fixture->guard, 1);
    fixture->nested_chain =
        uvdb_exception_guard_begin_chain(&fixture->guard, 1);
    __atomic_store_n(&fixture->predecessor_active, 1u,
                     __ATOMIC_RELEASE);
    (void)wait_flag(&fixture->release_predecessor);
    if(fixture->nested_chain == 1)
        uvdb_exception_guard_end_chain(&fixture->guard, 1);
    fixture->nested_leave = uvdb_exception_guard_leave(
        &fixture->guard, 1, fixture->nested_enter);
    __atomic_store_n(&fixture->nested_done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

static void test_shutdown_interleaving(void)
{
    struct interleave_fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    uvdb_exception_guard_init(&fixture.guard);

    pthread_t primary_thread;
    pthread_t nested_thread;
    check(pthread_create(&primary_thread, NULL, primary_main, &fixture) == 0,
          "create fake primary handler thread");
    check(pthread_create(&nested_thread, NULL, nested_predecessor_main,
                         &fixture) == 0,
          "create fake predecessor handler thread");
    check(wait_flag(&fixture.predecessor_active),
          "nested predecessor reached controlled blocking point");

    /* Production publishes restored handler slots before closing admission.
     * This close models the following instruction plus a callback whose
     * address had already been copied by the kernel dispatcher. */
    uvdb_exception_guard_close(&fixture.guard);
    int late = uvdb_exception_guard_enter(&fixture.guard, 2);
    check(late == UVDB_EXCEPTION_GUARD_CLOSED,
          "already-dispatched callback after close is classified closed");
    struct uvdb_exception_guard_stats stats;
    check(uvdb_exception_guard_get_stats(&fixture.guard, &stats) == 0 &&
              stats.active_handlers == 3 && stats.closed_entries == 1 &&
              stats.closing == 1,
          "closed callback still pins immutable teardown state");
    check(uvdb_exception_guard_leave(&fixture.guard, 2, late) == 0,
          "closed callback retires its counted lifetime");

    __atomic_store_n(&fixture.release_primary, 1u, __ATOMIC_RELEASE);
    check(wait_flag(&fixture.primary_done),
          "primary handler returned while predecessor stayed blocked");
    check(uvdb_exception_guard_get_stats(&fixture.guard, &stats) == 0 &&
              stats.active_handlers == 1,
          "nested predecessor lifetime remains after primary return");
    check(!uvdb_exception_guard_is_idle(&fixture.guard) &&
              uvdb_exception_guard_begin_chain(
                  &fixture.guard, 1) == 0,
          "primary leave neither reports idle nor clears peer chain owner");
    check(uvdb_exception_guard_reopen(&fixture.guard) < 0,
          "closed guard cannot reopen around active predecessor");

    __atomic_store_n(&fixture.release_predecessor, 1u, __ATOMIC_RELEASE);
    check(wait_flag(&fixture.nested_done),
          "nested predecessor retired after release");
    check(pthread_join(primary_thread, NULL) == 0 &&
              pthread_join(nested_thread, NULL) == 0,
          "join fake handler threads");
    check(fixture.primary_enter == UVDB_EXCEPTION_GUARD_PRIMARY &&
              fixture.nested_enter == UVDB_EXCEPTION_GUARD_NESTED &&
              fixture.nested_chain == 1 && fixture.primary_leave == 0 &&
              fixture.nested_leave == 0,
          "fake handler interleaving completed exact ownership transitions");
    check(uvdb_exception_guard_is_idle(&fixture.guard),
          "closed guard reaches visible quiescence after every callback");
    check(uvdb_exception_guard_reopen(&fixture.guard) == 0,
          "portable gate can reopen only after complete quiescence");
}

int main(void)
{
    test_basic_policy();
    test_shutdown_interleaving();
    if(failures)
        return 1;
    puts("PASS: concurrent exception lifetime, close, and predecessor policy");
    return 0;
}
