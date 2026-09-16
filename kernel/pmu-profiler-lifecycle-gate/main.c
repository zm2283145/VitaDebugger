#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>

#include "debugScreen.h"
#include "journal.h"
#include "vitadebug_pmu_profiler.h"

#define GATE_CORE_AFFINITY 0x00010000
#define GATE_WORK_WORDS 4096u
#define GATE_WORK_ROUNDS 24u
#define GATE_THREAD_STACK (16u * 1024u)
#define GATE_THREAD_WAIT_US 2000000u
#define GATE_REARM_POLLS 100u
#define GATE_REARM_POLL_US 20000u
#define GATE_WATCHDOG_WAIT_US 600000u
#define GATE_ENOENT UINT32_C(0x80010002)
#define GATE_EEXIST UINT32_C(0x80010011)

static volatile uint32_t gate_work[GATE_WORK_WORDS];
static volatile uint32_t gate_sink;

static volatile int32_t contender_open_result;
static volatile int32_t contender_cleanup_result;
static volatile int32_t exit_owner_affinity_result;
static volatile int32_t exit_owner_open_result;
static volatile int32_t exit_owner_read_result;
static struct vd_kernel_pmu_profiler_handle exit_owner_handle;
static struct vd_kernel_pmu_profiler_sample exit_owner_sample;

static const char* const record_paths[VD_PMU_LIFECYCLE_SLOT_COUNT] = {
    VD_PMU_LIFECYCLE_RECORD_A_PATH,
    VD_PMU_LIFECYCLE_RECORD_B_PATH,
    VD_PMU_LIFECYCLE_RECORD_C_PATH,
    VD_PMU_LIFECYCLE_RECORD_D_PATH,
    VD_PMU_LIFECYCLE_RECORD_E_PATH,
};

static int bytes_equal(const void* left, const void* right, size_t size)
{
    const uint8_t* lhs = (const uint8_t*)left;
    const uint8_t* rhs = (const uint8_t*)right;
    for(size_t i = 0; i < size; ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static void initialize_record(struct vd_pmu_lifecycle_record* record)
{
    uint8_t* bytes = (uint8_t*)record;
    for(size_t i = 0; i < sizeof(*record); ++i)
        bytes[i] = 0;
    int32_t* result = &record->info_result;
    for(size_t i = 0; i < 31u; ++i)
        result[i] = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record->journal_result = 0;
}

static int inspect_slot(
    size_t slot, struct vd_pmu_lifecycle_record* record,
    int* present, int* valid)
{
    if(slot >= VD_PMU_LIFECYCLE_SLOT_COUNT)
        return -1;
    SceIoStat stat;
    *present = 0;
    *valid = 0;
    int result = sceIoGetstat(record_paths[slot], &stat);
    if(result < 0)
        return (uint32_t)result == GATE_ENOENT ? 0 : result;
    *present = 1;
    if(stat.st_size != (SceOff)sizeof(*record))
        return 0;
    SceUID fd = sceIoOpen(record_paths[slot], SCE_O_RDONLY, 0);
    if(fd < 0)
        return fd;
    result = sceIoRead(fd, record, sizeof(*record));
    const int close_result = sceIoClose(fd);
    if(result != (int)sizeof(*record))
        return result < 0 ? result : -1;
    if(close_result < 0)
        return close_result;
    *valid = vdPmuLifecycleRecordValid(record);
    return 0;
}

static int read_latest(
    struct vd_pmu_lifecycle_record* latest, int* latest_slot)
{
    struct vd_pmu_lifecycle_record records[VD_PMU_LIFECYCLE_SLOT_COUNT];
    int present[VD_PMU_LIFECYCLE_SLOT_COUNT] = {0};
    int valid[VD_PMU_LIFECYCLE_SLOT_COUNT] = {0};
    for(size_t i = 0; i < VD_PMU_LIFECYCLE_SLOT_COUNT; ++i)
    {
        int result = inspect_slot(i, &records[i], &present[i], &valid[i]);
        if(result < 0)
            return result;
    }
    const int selected = vdPmuLifecycleSelectLatest(
        present, valid, records, VD_PMU_LIFECYCLE_SLOT_COUNT);
    if(selected < 0)
        return selected;
    *latest = records[selected];
    *latest_slot = selected;
    return 0;
}

static int write_slot_verified(
    size_t slot, struct vd_pmu_lifecycle_record* record)
{
    if(slot >= VD_PMU_LIFECYCLE_SLOT_COUNT)
        return -1;
    struct vd_pmu_lifecycle_record verification;
    record->magic = VD_PMU_LIFECYCLE_RECORD_MAGIC;
    record->version = VD_PMU_LIFECYCLE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuLifecycleChecksum(record);

    SceUID fd = sceIoOpen(record_paths[slot],
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL,
                          0666);
    if(fd < 0)
        return fd;
    int result = sceIoWrite(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
        result = sceIoSyncByFd(fd, 0);
    else if(result >= 0)
        result = -1;
    const int close_result = sceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result >= 0)
        result = sceIoSync("ux0:", 0);
    if(result < 0)
        return result;

    int present = 0;
    int valid = 0;
    result = inspect_slot(slot, &verification, &present, &valid);
    if(result < 0 || !present || !valid ||
       !bytes_equal(&verification, record, sizeof(verification)))
        return result < 0 ? result : -1;
    return 0;
}

__attribute__((noinline))
static void run_bounded_workload(void)
{
    uint32_t value = UINT32_C(0x13579bdf);
    for(uint32_t round = 0; round < GATE_WORK_ROUNDS; ++round)
    {
        for(uint32_t i = 0; i < GATE_WORK_WORDS; i += 17u)
        {
            value ^= gate_work[(i + round * 31u) &
                               (GATE_WORK_WORDS - 1u)];
            if((value & 7u) == (round & 7u))
                value = (value << 5) | (value >> 27);
            else
                value += i ^ round;
        }
    }
    gate_sink = value;
}

static struct vd_kernel_pmu_profiler_open_request make_request(
    uint32_t event_code, uint32_t lease_ms)
{
    const struct vd_kernel_pmu_profiler_open_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
        .event_code = event_code,
        .lease_ms = lease_ms,
        .flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT,
    };
    return request;
}

static int sample_valid(
    const struct vd_kernel_pmu_profiler_handle* handle,
    const struct vd_kernel_pmu_profiler_sample* sample,
    uint32_t event_code)
{
    return handle && sample &&
        sample->struct_size == sizeof(*sample) &&
        sample->abi_version == VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
        sample->owner_token == handle->owner_token &&
        sample->generation == handle->generation &&
        sample->event_code == event_code &&
        sample->core_id == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
        sample->physical_counter ==
            VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
        sample->flags == 0 && sample->reserved[0] == 0 &&
        sample->reserved[1] == 0;
}

static int info_has_event(
    const struct vd_kernel_pmu_profiler_info* info, uint32_t event_code)
{
    if(!info || info->event_count > VD_KERNEL_PMU_PROFILER_MAX_EVENTS)
        return 0;
    for(uint32_t i = 0; i < info->event_count; ++i)
        if(info->event_codes[i] == event_code)
            return 1;
    return 0;
}

static int info_ready(
    int result, const struct vd_kernel_pmu_profiler_info* info)
{
    const uint32_t required = VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE |
        VD_KERNEL_PMU_PROFILER_CAP_LEASE_WATCHDOG |
        VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS |
        VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM;
    return result == 0 && info &&
        (info->capabilities & required) == required &&
        (info->capabilities &
         VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT) == 0 &&
        info->fixed_core == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
        info->fixed_counter == VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
        info->min_lease_ms == VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS &&
        info->max_lease_ms == VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS &&
        info_has_event(info, VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
        info_has_event(info, VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS) &&
        info_has_event(info,
                       VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT);
}

static int contender_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    struct vd_kernel_pmu_profiler_open_request request = make_request(
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
        VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    struct vd_kernel_pmu_profiler_handle handle = {0};
    contender_cleanup_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    contender_open_result = vdKernelPmuProfilerOpen(&request, &handle);
    if(contender_open_result == 0)
        contender_cleanup_result = vdKernelPmuProfilerClose(&handle);
    return 0;
}

static int exit_owner_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    exit_owner_affinity_result = sceKernelChangeThreadCpuAffinityMask(
        sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    exit_owner_open_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    exit_owner_read_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    if(exit_owner_affinity_result >= 0)
    {
        struct vd_kernel_pmu_profiler_open_request request = make_request(
            VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
            VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
        exit_owner_open_result = vdKernelPmuProfilerOpen(
            &request, &exit_owner_handle);
        if(exit_owner_open_result == 0)
        {
            run_bounded_workload();
            exit_owner_read_result = vdKernelPmuProfilerRead(
                &exit_owner_handle, &exit_owner_sample);
        }
    }
    /* Deliberately return without Close.  The kernel must identify this exact
     * retained thread object as dormant/gone and restore before the 5 s
     * lease timeout. */
    return 0;
}

static int write_failure(
    struct vd_pmu_lifecycle_record* record, size_t slot,
    uint32_t revision)
{
    record->revision = revision;
    record->state = VD_PMU_LIFECYCLE_FAILED;
    record->flags &= ~VD_PMU_LIFECYCLE_FLAG_PASS;
    record->timestamp_us = sceKernelGetProcessTimeWide();
    record->journal_result = 0;
    return write_slot_verified(slot, record);
}

static void print_header(
    int info_result, const struct vd_kernel_pmu_profiler_info* info,
    int journal_result, const struct vd_pmu_lifecycle_record* latest)
{
    psvDebugScreenPrintf("VitaDebugger PMU lifecycle gate\n\n");
    psvDebugScreenPrintf(
        "Core 0 / lane 5 / retained-owner safe re-arm\n");
    psvDebugScreenPrintf("Opening this UI performs no PMU access.\n\n");
    psvDebugScreenPrintf("GetInfo: %08X\n", (uint32_t)info_result);
    if(info_result == 0)
        psvDebugScreenPrintf("ABI=%u caps=%08X lease=%u..%u ms\n",
                             info->abi_version, info->capabilities,
                             info->min_lease_ms, info->max_lease_ms);
    if(journal_result == VD_PMU_LIFECYCLE_SELECT_EMPTY)
    {
        psvDebugScreenPrintf("Journal: empty; first launch ready\n\n");
        psvDebugScreenPrintf(
            "X: run close/contention/timeout/thread-exit stages\n");
        psvDebugScreenPrintf(
            "The app will then exit with a live 5 s lease.\n");
    }
    else if(journal_result == 0 && latest &&
            latest->state == VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED)
    {
        psvDebugScreenPrintf(
            "Journal: process-exit lease armed; relaunch stage ready\n\n");
        psvDebugScreenPrintf(
            "X: prove process-exit cleanup and same-boot re-arm\n");
    }
    else if(journal_result == 0 && latest)
    {
        psvDebugScreenPrintf(
            "Journal: LOCKED rev=%u state=%u flags=%08X\n",
            latest->revision, latest->state, latest->flags);
        psvDebugScreenPrintf(
            "Archive every lifecycle slot; do not erase ambiguity.\n");
    }
    else
        psvDebugScreenPrintf("Journal: LOCKED (%08X)\n",
                             (uint32_t)journal_result);
    psvDebugScreenPrintf("O: exit without running\n");
}

static void run_first_stage(
    int info_result, const struct vd_kernel_pmu_profiler_info* info)
{
    const int affinity_before = sceKernelGetThreadCpuAffinityMask(
        sceKernelGetThreadId());
    const int affinity_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    struct vd_pmu_lifecycle_record record;
    initialize_record(&record);
    record.revision = 1;
    record.state = VD_PMU_LIFECYCLE_ATTEMPTED;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    record.info_result = info_result;
    record.affinity_result = affinity_result;
    record.capabilities = info_result == 0 ? info->capabilities : 0;

    if(!info_ready(info_result, info) || affinity_result < 0 ||
       write_slot_verified(0, &record) < 0)
    {
        if(affinity_before >= 0)
            record.affinity_restore_result =
                sceKernelChangeThreadCpuAffinityMask(
                    sceKernelGetThreadId(), affinity_before);
        psvDebugScreenPrintf(
            "BLOCKED before PMU access: info/affinity/journal gate failed.\n");
        return;
    }
    psvDebugScreenPrintf("Attempt journal verified. Running stages...\n");

    struct vd_kernel_pmu_profiler_open_request request = make_request(
        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
        VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    struct vd_kernel_pmu_profiler_handle handle = {0};
    struct vd_kernel_pmu_profiler_sample sample = {0};
    record.explicit_open_result = vdKernelPmuProfilerOpen(
        &request, &handle);
    if(record.explicit_open_result == 0)
    {
        run_bounded_workload();
        record.explicit_read_result = vdKernelPmuProfilerRead(
            &handle, &sample);
        record.explicit_close_result = vdKernelPmuProfilerClose(&handle);
        record.explicit_owner_token = handle.owner_token;
        record.explicit_generation = handle.generation;
        record.explicit_value = sample.value;
    }
    if(record.explicit_open_result == 0 &&
       record.explicit_read_result == 0 &&
       sample_valid(&handle, &sample,
                    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
       record.explicit_close_result == 0)
        record.flags |= VD_PMU_LIFECYCLE_FLAG_EXPLICIT_CLOSE;

    request = make_request(VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS,
                           VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    handle = (struct vd_kernel_pmu_profiler_handle){0};
    record.timeout_open_result = vdKernelPmuProfilerOpen(
        &request, &handle);
    record.timeout_owner_token = handle.owner_token;
    record.timeout_generation = handle.generation;
    if(record.timeout_open_result == 0)
    {
        contender_open_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
        contender_cleanup_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
        SceUID contender = sceKernelCreateThread(
            "vd pmu contender", contender_main, 0x40,
            GATE_THREAD_STACK, 0, 0, NULL);
        record.contender_create_result = contender;
        if(contender >= 0)
        {
            record.contender_start_result = sceKernelStartThread(
                contender, 0, NULL);
            if(record.contender_start_result >= 0)
            {
                SceUInt wait_us = GATE_THREAD_WAIT_US;
                int status = 0;
                record.contender_wait_result = sceKernelWaitThreadEnd(
                    contender, &status, &wait_us);
            }
            record.contender_open_result = contender_open_result;
            record.contender_cleanup_result = contender_cleanup_result;
            record.contender_delete_result =
                sceKernelDeleteThread(contender);
        }

        sceKernelDelayThread(GATE_WATCHDOG_WAIT_US);
        sample = (struct vd_kernel_pmu_profiler_sample){0};
        record.timeout_read_result = vdKernelPmuProfilerRead(
            &handle, &sample);
        record.timeout_close_result = vdKernelPmuProfilerClose(&handle);
    }
    if(record.contender_create_result >= 0 &&
       record.contender_start_result >= 0 &&
       record.contender_wait_result >= 0 &&
       record.contender_delete_result >= 0 &&
       record.contender_open_result == VD_KERNEL_ERROR_PMU_PROFILER_BUSY &&
       record.contender_cleanup_result ==
           VD_PMU_LIFECYCLE_RESULT_NOT_RUN)
        record.flags |= VD_PMU_LIFECYCLE_FLAG_OWNERSHIP_CONFLICT;
    if(record.timeout_open_result == 0 &&
       record.timeout_read_result ==
           VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
       record.timeout_close_result == 0)
        record.flags |= VD_PMU_LIFECYCLE_FLAG_TIMEOUT_WATCHDOG;

    exit_owner_affinity_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    exit_owner_open_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    exit_owner_read_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    exit_owner_handle = (struct vd_kernel_pmu_profiler_handle){0};
    exit_owner_sample = (struct vd_kernel_pmu_profiler_sample){0};
    SceUID exit_owner = sceKernelCreateThread(
        "vd pmu exit owner", exit_owner_main, 0x40,
        GATE_THREAD_STACK, 0, 0, NULL);
    record.exit_owner_create_result = exit_owner;
    if(exit_owner >= 0)
    {
        record.exit_owner_start_result = sceKernelStartThread(
            exit_owner, 0, NULL);
        if(record.exit_owner_start_result >= 0)
        {
            SceUInt wait_us = GATE_THREAD_WAIT_US;
            int status = 0;
            record.exit_owner_wait_result = sceKernelWaitThreadEnd(
                exit_owner, &status, &wait_us);
        }
        record.exit_owner_affinity_result = exit_owner_affinity_result;
        record.exit_owner_open_result = exit_owner_open_result;
        record.exit_owner_read_result = exit_owner_read_result;
        record.exit_owner_token = exit_owner_handle.owner_token;
        record.exit_generation = exit_owner_handle.generation;
        record.exit_value = exit_owner_sample.value;

        request = make_request(
            VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
            VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
        struct vd_kernel_pmu_profiler_handle post_handle = {0};
        record.post_exit_open_result =
            VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
        for(uint32_t i = 0; i < GATE_REARM_POLLS; ++i)
        {
            record.post_exit_open_result = vdKernelPmuProfilerOpen(
                &request, &post_handle);
            if(record.post_exit_open_result !=
               VD_KERNEL_ERROR_PMU_PROFILER_BUSY)
                break;
            sceKernelDelayThread(GATE_REARM_POLL_US);
        }
        struct vd_kernel_pmu_profiler_sample post_sample = {0};
        if(record.post_exit_open_result == 0)
        {
            run_bounded_workload();
            record.post_exit_read_result = vdKernelPmuProfilerRead(
                &post_handle, &post_sample);
            record.post_exit_close_result = vdKernelPmuProfilerClose(
                &post_handle);
            record.post_exit_owner_token = post_handle.owner_token;
            record.post_exit_generation = post_handle.generation;
            record.post_exit_value = post_sample.value;
        }
        record.exit_owner_delete_result =
            sceKernelDeleteThread(exit_owner);
        if(record.exit_owner_affinity_result >= 0 &&
           record.exit_owner_open_result == 0 &&
           record.exit_owner_read_result == 0 &&
           sample_valid(&exit_owner_handle, &exit_owner_sample,
                        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
           record.exit_owner_wait_result >= 0 &&
           record.exit_owner_delete_result >= 0 &&
           record.post_exit_open_result == 0 &&
           record.post_exit_read_result == 0 &&
           sample_valid(&post_handle, &post_sample,
                        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
           record.post_exit_close_result == 0)
            record.flags |= VD_PMU_LIFECYCLE_FLAG_THREAD_EXIT;
    }

    if((record.flags & VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS) !=
       VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS)
    {
        if(affinity_before >= 0)
            record.affinity_restore_result =
                sceKernelChangeThreadCpuAffinityMask(
                    sceKernelGetThreadId(), affinity_before);
        const int journal = write_failure(&record, 1, 2);
        psvDebugScreenPrintf(
            "FAIL before process-exit stage; journal=%08X flags=%08X\n",
            (uint32_t)journal, record.flags);
        return;
    }

    record.revision = 2;
    record.state = VD_PMU_LIFECYCLE_PROCESS_EXIT_ATTEMPTED;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    if(write_slot_verified(1, &record) < 0)
    {
        if(affinity_before >= 0)
            record.affinity_restore_result =
                sceKernelChangeThreadCpuAffinityMask(
                    sceKernelGetThreadId(), affinity_before);
        psvDebugScreenPrintf(
            "BLOCKED: process-exit attempt journal failed before Open.\n");
        return;
    }

    request = make_request(VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS,
                           VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
    handle = (struct vd_kernel_pmu_profiler_handle){0};
    sample = (struct vd_kernel_pmu_profiler_sample){0};
    record.process_open_result = vdKernelPmuProfilerOpen(&request, &handle);
    if(record.process_open_result == 0)
    {
        run_bounded_workload();
        record.process_read_result = vdKernelPmuProfilerRead(
            &handle, &sample);
        record.process_owner_token = handle.owner_token;
        record.process_generation = handle.generation;
        record.process_value = sample.value;
    }
    if(record.process_open_result != 0 ||
       record.process_read_result != 0 ||
       !sample_valid(&handle, &sample,
                     VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS))
    {
        if(record.process_open_result == 0)
            record.process_cleanup_result =
                vdKernelPmuProfilerClose(&handle);
        if(affinity_before >= 0)
            record.affinity_restore_result =
                sceKernelChangeThreadCpuAffinityMask(
                    sceKernelGetThreadId(), affinity_before);
        const int journal = write_failure(&record, 2, 3);
        psvDebugScreenPrintf(
            "FAIL arming process exit; cleanup=%08X journal=%08X\n",
            (uint32_t)record.process_cleanup_result,
            (uint32_t)journal);
        return;
    }

    record.revision = 3;
    record.state = VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED;
    record.flags |= VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    if(write_slot_verified(2, &record) < 0)
    {
        record.process_cleanup_result = vdKernelPmuProfilerClose(&handle);
        psvDebugScreenPrintf(
            "BLOCKED: armed journal failed; emergency close=%08X\n",
            (uint32_t)record.process_cleanup_result);
        return;
    }

    psvDebugScreenPrintf(
        "First stage PASS. Exiting with live owner lease now.\n");
    psvDebugScreenPrintf(
        "Relaunch this gate and press X for the final proof.\n");
    const int exit_result = sceKernelExitProcess(0);
    record.process_cleanup_result = vdKernelPmuProfilerClose(&handle);
    record.affinity_restore_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), affinity_before);
    const int journal = write_failure(&record, 3, 4);
    psvDebugScreenPrintf(
        "Unexpected ExitProcess return=%08X close=%08X journal=%08X\n",
        (uint32_t)exit_result, (uint32_t)record.process_cleanup_result,
        (uint32_t)journal);
}

static void run_resume_stage(
    int info_result, const struct vd_kernel_pmu_profiler_info* info,
    const struct vd_pmu_lifecycle_record* armed)
{
    const int affinity_before = sceKernelGetThreadCpuAffinityMask(
        sceKernelGetThreadId());
    const int affinity_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    struct vd_pmu_lifecycle_record record = *armed;
    record.revision = 4;
    record.state = VD_PMU_LIFECYCLE_RESUME_ATTEMPTED;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    record.info_result = info_result;
    record.affinity_result = affinity_result;
    record.capabilities = info_result == 0 ? info->capabilities : 0;
    record.resume_open_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record.resume_read_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record.resume_close_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record.affinity_restore_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record.resume_owner_token = 0;
    record.resume_generation = 0;
    record.resume_value = 0;
    record.flags &= ~(VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM |
                      VD_PMU_LIFECYCLE_FLAG_PASS);

    if(!info_ready(info_result, info) || affinity_result < 0 ||
       write_slot_verified(3, &record) < 0)
    {
        if(affinity_before >= 0)
            (void)sceKernelChangeThreadCpuAffinityMask(
                sceKernelGetThreadId(), affinity_before);
        psvDebugScreenPrintf(
            "BLOCKED before resume PMU access: prerequisite failed.\n");
        return;
    }

    struct vd_kernel_pmu_profiler_open_request request = make_request(
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
        VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    struct vd_kernel_pmu_profiler_handle handle = {0};
    record.resume_open_result = VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
    for(uint32_t i = 0; i < GATE_REARM_POLLS; ++i)
    {
        record.resume_open_result = vdKernelPmuProfilerOpen(
            &request, &handle);
        if(record.resume_open_result != VD_KERNEL_ERROR_PMU_PROFILER_BUSY)
            break;
        sceKernelDelayThread(GATE_REARM_POLL_US);
    }
    struct vd_kernel_pmu_profiler_sample sample = {0};
    if(record.resume_open_result == 0)
    {
        run_bounded_workload();
        record.resume_read_result = vdKernelPmuProfilerRead(
            &handle, &sample);
        record.resume_close_result = vdKernelPmuProfilerClose(&handle);
        record.resume_owner_token = handle.owner_token;
        record.resume_generation = handle.generation;
        record.resume_value = sample.value;
    }
    record.affinity_restore_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), affinity_before);

    const int passed = record.resume_open_result == 0 &&
        record.resume_read_result == 0 &&
        sample_valid(&handle, &sample,
                     VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
        record.resume_close_result == 0 &&
        record.affinity_restore_result >= 0;
    record.revision = 5;
    record.state = passed ? VD_PMU_LIFECYCLE_COMPLETE :
                            VD_PMU_LIFECYCLE_FAILED;
    if(passed)
        record.flags |= VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM |
                        VD_PMU_LIFECYCLE_FLAG_PASS;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    const int journal = write_slot_verified(4, &record);
    psvDebugScreenPrintf(
        "resume open=%08X read=%08X close=%08X affinity=%08X\n",
        (uint32_t)record.resume_open_result,
        (uint32_t)record.resume_read_result,
        (uint32_t)record.resume_close_result,
        (uint32_t)record.affinity_restore_result);
    psvDebugScreenPrintf("completion journal=%08X\n", (uint32_t)journal);
    psvDebugScreenPrintf("Lifecycle gate: %s\n",
                         passed && journal == 0 ? "PASS" : "FAIL");
    psvDebugScreenPrintf(
        "Archive all five journal slots and preserve a screenshot.\n");
}

int main(void)
{
    if(psvDebugScreenInit() < 0)
        return 1;
    const int mkdir_result = sceIoMkdir("ux0:data/VitaDebugger", 0777);
    struct vd_pmu_lifecycle_record latest;
    int latest_slot = -1;
    int journal_result =
        mkdir_result < 0 && (uint32_t)mkdir_result != GATE_EEXIST ?
            mkdir_result : read_latest(&latest, &latest_slot);
    struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    const int info_result = vdKernelPmuProfilerGetInfo(&info);
    print_header(info_result, &info, journal_result,
                 journal_result == 0 ? &latest : NULL);

    SceCtrlData previous = {0};
    int ran = 0;
    for(;;)
    {
        SceCtrlData pad = {0};
        if(sceCtrlPeekBufferPositive(0, &pad, 1) <= 0)
        {
            sceKernelDelayThread(20000);
            continue;
        }
        const uint32_t pressed = pad.buttons & ~previous.buttons;
        previous = pad;
        if((pressed & SCE_CTRL_CIRCLE) != 0)
            break;
        if((pressed & SCE_CTRL_CROSS) == 0 || ran)
        {
            sceKernelDelayThread(20000);
            continue;
        }
        ran = 1;
        if(journal_result == VD_PMU_LIFECYCLE_SELECT_EMPTY)
            run_first_stage(info_result, &info);
        else if(journal_result == 0 && latest_slot == 2 &&
                latest.state == VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED)
            run_resume_stage(info_result, &info, &latest);
        else
            psvDebugScreenPrintf(
                "LOCKED: journal is not a runnable lifecycle state.\n");
        psvDebugScreenPrintf("O: exit\n");
    }
    (void)psvDebugScreenFinish();
    return 0;
}
