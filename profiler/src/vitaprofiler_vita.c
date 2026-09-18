#include "vitaprofiler.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

_Static_assert(sizeof(SceKernelSysClock) == sizeof(uint64_t),
               "runClocks source storage must remain 64-bit");

static uint64_t vp_vita_clock(void* user)
{
    SceInt64 now;
    (void)user;
    now = sceKernelGetSystemTimeWide();
    return now < 0 ? 0u : (uint64_t)now;
}

static uint32_t vp_vita_thread_id(void* user)
{
    (void)user;
    return (uint32_t)sceKernelGetThreadId();
}

static int vp_record_sample(struct vp_context* context, uint64_t timestamp_us,
                            uint32_t thread_id, uint16_t type,
                            uint16_t flags, uint32_t name_id, int64_t value)
{
    struct vp_event event;
    event.timestamp_us = timestamp_us;
    event.value = value;
    event.name_id = name_id;
    event.thread_id = thread_id;
    event.correlation_id = 0u;
    event.type = type;
    event.flags = flags;
    return vp_record(context, &event);
}

static int vp_merge_record_result(int aggregate, int result)
{
    if (result < 0)
        return result;
    if (result == VP_RESULT_DROPPED)
        return VP_RESULT_DROPPED;
    return aggregate;
}

static int64_t vp_raw_u64_as_i64(uint64_t value)
{
    int64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

int vp_vita_init(struct vp_context* context, struct vp_slot* slots,
                 uint32_t capacity)
{
    struct vp_config config;
    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = capacity;
    config.clock = vp_vita_clock;
    config.thread_id = vp_vita_thread_id;
    return vp_init(context, &config);
}

int vp_vita_capture_memory(struct vp_vita_memory_snapshot* snapshot)
{
    SceKernelFreeMemorySizeInfo memory;
    SceInt64 now;
    if (snapshot == NULL)
        return VP_ERROR_INVALID_ARGUMENT;

    memset(snapshot, 0, sizeof(*snapshot));
    memset(&memory, 0, sizeof(memory));
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) < 0)
        return VP_ERROR_PLATFORM;

    now = sceKernelGetSystemTimeWide();
    if (now < 0)
        return VP_ERROR_PLATFORM;
    snapshot->timestamp_us = (uint64_t)now;
    snapshot->process_time_us = (uint64_t)sceKernelGetProcessTimeWide();
    snapshot->free_user_bytes = (uint32_t)memory.size_user;
    snapshot->free_cdram_bytes = (uint32_t)memory.size_cdram;
    snapshot->free_phycont_bytes = (uint32_t)memory.size_phycont;
    return VP_RESULT_OK;
}

int vp_vita_record_memory(struct vp_context* context,
                          struct vp_vita_memory_snapshot* snapshot)
{
    struct vp_vita_memory_snapshot captured;
    uint32_t thread_id;
    int aggregate = VP_RESULT_OK;
    int result;

    result = vp_vita_capture_memory(&captured);
    if (result != VP_RESULT_OK)
        return result;
    if (snapshot != NULL)
        *snapshot = captured;
    thread_id = (uint32_t)sceKernelGetThreadId();

    result = vp_record_sample(context, captured.timestamp_us, thread_id,
                              VP_EVENT_MEMORY_SAMPLE, VP_EVENT_FLAG_NONE,
                              VP_METRIC_FREE_USER_BYTES,
                              (int64_t)captured.free_user_bytes);
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us, thread_id,
                              VP_EVENT_MEMORY_SAMPLE, VP_EVENT_FLAG_NONE,
                              VP_METRIC_FREE_CDRAM_BYTES,
                              (int64_t)captured.free_cdram_bytes);
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us, thread_id,
                              VP_EVENT_MEMORY_SAMPLE, VP_EVENT_FLAG_NONE,
                              VP_METRIC_FREE_PHYCONT_BYTES,
                              (int64_t)captured.free_phycont_bytes);
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us, thread_id,
                              VP_EVENT_PROCESS_SAMPLE, VP_EVENT_FLAG_NONE,
                              VP_METRIC_PROCESS_TIME_US,
                              (int64_t)captured.process_time_us);
    return vp_merge_record_result(aggregate, result);
}

int vp_vita_capture_thread(uint32_t thread_id,
                           struct vp_vita_thread_snapshot* snapshot)
{
    SceKernelThreadInfo info;
    SceInt64 now;
    int stack_free;
    SceUID target;

    if (snapshot == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(snapshot, 0, sizeof(*snapshot));

    target = thread_id == 0u ? sceKernelGetThreadId() : (SceUID)thread_id;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(target, &info) < 0)
        return VP_ERROR_PLATFORM;
    stack_free = sceKernelGetThreadStackFreeSize(target);
    if (stack_free < 0)
        return VP_ERROR_PLATFORM;
    now = sceKernelGetSystemTimeWide();
    if (now < 0)
        return VP_ERROR_PLATFORM;

    snapshot->timestamp_us = (uint64_t)now;
    snapshot->run_clocks = (uint64_t)info.runClocks;
    snapshot->thread_id = (uint32_t)target;
    snapshot->stack_free_bytes = stack_free;
    snapshot->thread_preemptions = info.threadPreemptCount;
    snapshot->interrupt_preemptions = info.intrPreemptCount;
    return VP_RESULT_OK;
}

int vp_vita_record_thread(struct vp_context* context, uint32_t thread_id,
                          struct vp_vita_thread_snapshot* snapshot)
{
    struct vp_vita_thread_snapshot captured;
    int aggregate = VP_RESULT_OK;
    int result;

    result = vp_vita_capture_thread(thread_id, &captured);
    if (result != VP_RESULT_OK)
        return result;
    if (snapshot != NULL)
        *snapshot = captured;

    result = vp_record_sample(context, captured.timestamp_us,
                              captured.thread_id, VP_EVENT_THREAD_SAMPLE,
                              VP_EVENT_FLAG_RAW_VALUE,
                              VP_METRIC_THREAD_RUN_CLOCKS,
                              vp_raw_u64_as_i64(captured.run_clocks));
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us,
                              captured.thread_id, VP_EVENT_THREAD_SAMPLE,
                              VP_EVENT_FLAG_NONE,
                              VP_METRIC_THREAD_STACK_FREE_BYTES,
                              (int64_t)captured.stack_free_bytes);
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us,
                              captured.thread_id, VP_EVENT_THREAD_SAMPLE,
                              VP_EVENT_FLAG_RAW_VALUE,
                              VP_METRIC_THREAD_PREEMPTIONS,
                              (int64_t)captured.thread_preemptions);
    aggregate = vp_merge_record_result(aggregate, result);
    if (aggregate < 0)
        return aggregate;
    result = vp_record_sample(context, captured.timestamp_us,
                              captured.thread_id, VP_EVENT_THREAD_SAMPLE,
                              VP_EVENT_FLAG_RAW_VALUE,
                              VP_METRIC_INTERRUPT_PREEMPTIONS,
                              (int64_t)captured.interrupt_preemptions);
    return vp_merge_record_result(aggregate, result);
}
