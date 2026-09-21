#include <stdint.h>
#include <stdio.h>

#include "pmu_process_event_pending.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int failures;

#define CHECK(condition, message)                                      \
    do                                                                 \
    {                                                                  \
        if(!(condition))                                               \
        {                                                              \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                \
        }                                                              \
    } while(0)

struct publish_args {
    volatile uint32_t* pending;
    int32_t pid;
};

#ifdef _WIN32
static DWORD WINAPI publish_thread(void* context)
#else
static void* publish_thread(void* context)
#endif
{
    struct publish_args* args = (struct publish_args*)context;
    (void)vdPmuProcessEventPublish(args->pending, args->pid);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int publish_pair(
    volatile uint32_t* pending, int32_t left_pid, int32_t right_pid)
{
    struct publish_args left = {pending, left_pid};
    struct publish_args right = {pending, right_pid};
#ifdef _WIN32
    HANDLE threads[2] = {
        CreateThread(NULL, 0, publish_thread, &left, 0, NULL),
        CreateThread(NULL, 0, publish_thread, &right, 0, NULL),
    };
    const int created = threads[0] != NULL && threads[1] != NULL;
    if(threads[0])
    {
        (void)WaitForSingleObject(threads[0], INFINITE);
        CloseHandle(threads[0]);
    }
    if(threads[1])
    {
        (void)WaitForSingleObject(threads[1], INFINITE);
        CloseHandle(threads[1]);
    }
    return created;
#else
    pthread_t threads[2];
    const int left_created =
        pthread_create(&threads[0], NULL, publish_thread, &left) == 0;
    const int right_created =
        pthread_create(&threads[1], NULL, publish_thread, &right) == 0;
    if(left_created)
        (void)pthread_join(threads[0], NULL);
    if(right_created)
        (void)pthread_join(threads[1], NULL);
    return left_created && right_created;
#endif
}

int main(void)
{
    volatile uint32_t pending = VD_PMU_PROCESS_EVENT_EMPTY;
    int32_t pid = -1;
    CHECK(vdPmuProcessEventTake(&pending) ==
              VD_PMU_PROCESS_EVENT_EMPTY &&
              vdPmuProcessEventPublish(&pending, 41) == 42 &&
              vdPmuProcessEventDecode(
                  vdPmuProcessEventTake(&pending), &pid) &&
              pid == 41,
          "an event published after an empty take remains observable");
    CHECK(vdPmuProcessEventPublish(&pending, 41) == 42 &&
              vdPmuProcessEventPublish(&pending, 41) == 42 &&
              vdPmuProcessEventTake(&pending) == 42,
          "same-PID callbacks coalesce without false ambiguity");
    CHECK(vdPmuProcessEventPublish(&pending, 41) == 42 &&
              vdPmuProcessEventPublish(&pending, 42) ==
                  VD_PMU_PROCESS_EVENT_AMBIGUOUS &&
              vdPmuProcessEventPublish(&pending, 41) ==
                  VD_PMU_PROCESS_EVENT_AMBIGUOUS &&
              vdPmuProcessEventTake(&pending) ==
                  VD_PMU_PROCESS_EVENT_AMBIGUOUS,
          "distinct callbacks escalate and cannot downgrade ambiguity");

    for(int iteration = 0; iteration < 1000; ++iteration)
    {
        pending = VD_PMU_PROCESS_EVENT_EMPTY;
        CHECK(publish_pair(&pending, 51, 52),
              "distinct publisher threads start");
        CHECK(vdPmuProcessEventTake(&pending) ==
                  VD_PMU_PROCESS_EVENT_AMBIGUOUS,
              "concurrent distinct PIDs always escalate to ambiguity");

        pending = VD_PMU_PROCESS_EVENT_EMPTY;
        CHECK(publish_pair(&pending, 61, 61),
              "same-PID publisher threads start");
        const uint32_t same_pid = vdPmuProcessEventTake(&pending);
        CHECK(same_pid == vdPmuProcessEventEncode(61) ||
                  same_pid == VD_PMU_PROCESS_EVENT_AMBIGUOUS,
              "concurrent same-PID callbacks are retained or conservatively ambiguous");
    }

    if(failures != 0)
    {
        fprintf(stderr, "%d pending-event test(s) failed\n", failures);
        return 1;
    }
    puts("PASS: PMU process-event pending handoff");
    return 0;
}
