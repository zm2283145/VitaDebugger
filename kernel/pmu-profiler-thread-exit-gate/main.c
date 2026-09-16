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
#define GATE_REARM_POLL_US 20000u
#define GATE_RECOVERY_CEILING_US UINT64_C(6000000)
#define GATE_ENOENT UINT32_C(0x80010002)
#define GATE_EEXIST UINT32_C(0x80010011)

static volatile uint32_t gate_work[GATE_WORK_WORDS];
static volatile uint32_t gate_sink;

static volatile int32_t owner_affinity_result;
static volatile int32_t owner_open_result;
static volatile int32_t owner_read_result;
static volatile uint64_t owner_open_attempt_us;
static struct vd_kernel_pmu_profiler_handle owner_handle;
static struct vd_kernel_pmu_profiler_sample owner_sample;

static const char* const record_paths[VD_PMU_THREAD_EXIT_SLOT_COUNT] = {
    VD_PMU_THREAD_EXIT_RECORD_A_PATH,
    VD_PMU_THREAD_EXIT_RECORD_B_PATH,
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

static void initialize_record(struct vd_pmu_thread_exit_record* record)
{
    uint8_t* bytes = (uint8_t*)record;
    for(size_t i = 0; i < sizeof(*record); ++i)
        bytes[i] = 0;
    int32_t* result = &record->info_result;
    for(size_t i = 0; i < 14u; ++i)
        result[i] = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    record->journal_result = 0;
}

static int inspect_slot(
    size_t slot, struct vd_pmu_thread_exit_record* record,
    int* present, int* valid)
{
    if(slot >= VD_PMU_THREAD_EXIT_SLOT_COUNT)
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
    *valid = vdPmuThreadExitRecordValid(record);
    return 0;
}

static int read_latest(
    struct vd_pmu_thread_exit_record* latest, int* latest_slot)
{
    struct vd_pmu_thread_exit_record
        records[VD_PMU_THREAD_EXIT_SLOT_COUNT];
    int present[VD_PMU_THREAD_EXIT_SLOT_COUNT] = {0};
    int valid[VD_PMU_THREAD_EXIT_SLOT_COUNT] = {0};
    for(size_t i = 0; i < VD_PMU_THREAD_EXIT_SLOT_COUNT; ++i)
    {
        const int result = inspect_slot(
            i, &records[i], &present[i], &valid[i]);
        if(result < 0)
            return result;
    }
    const int selected = vdPmuThreadExitSelectLatest(
        present, valid, records, VD_PMU_THREAD_EXIT_SLOT_COUNT);
    if(selected < 0)
        return selected;
    *latest = records[selected];
    *latest_slot = selected;
    return 0;
}

static int write_slot_verified(
    size_t slot, struct vd_pmu_thread_exit_record* record)
{
    if(slot >= VD_PMU_THREAD_EXIT_SLOT_COUNT)
        return -1;
    struct vd_pmu_thread_exit_record verification;
    record->magic = VD_PMU_THREAD_EXIT_RECORD_MAGIC;
    record->version = VD_PMU_THREAD_EXIT_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuThreadExitChecksum(record);

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
        info_has_event(info,
                       VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
        info_has_event(info,
                       VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT);
}

static int owner_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    owner_affinity_result = sceKernelChangeThreadCpuAffinityMask(
        sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    owner_open_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    owner_read_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    if(owner_affinity_result >= 0)
    {
        struct vd_kernel_pmu_profiler_open_request request = make_request(
            VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
            VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
        owner_open_attempt_us = sceKernelGetProcessTimeWide();
        owner_open_result = vdKernelPmuProfilerOpen(
            &request, &owner_handle);
        if(owner_open_result == 0)
        {
            run_bounded_workload();
            owner_read_result = vdKernelPmuProfilerRead(
                &owner_handle, &owner_sample);
        }
    }
    /* Deliberately return without Close.  The gate keeps the process alive
     * and asks a different thread to prove dormant-owner cleanup and re-arm. */
    return 0;
}

static int run_gate(
    int info_result, const struct vd_kernel_pmu_profiler_info* info)
{
    const int affinity_before = sceKernelGetThreadCpuAffinityMask(
        sceKernelGetThreadId());
    const int affinity_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    struct vd_pmu_thread_exit_record record;
    initialize_record(&record);
    record.revision = 1;
    record.state = VD_PMU_THREAD_EXIT_ATTEMPTED;
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
        return 1;
    }
    psvDebugScreenPrintf(
        "Attempt journal verified. Running thread-only stage...\n");

    owner_affinity_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    owner_open_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    owner_read_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    owner_open_attempt_us = 0;
    owner_handle = (struct vd_kernel_pmu_profiler_handle){0};
    owner_sample = (struct vd_kernel_pmu_profiler_sample){0};
    SceUID owner = sceKernelCreateThread(
        "vd pmu thread-exit owner", owner_main, 0x40,
        GATE_THREAD_STACK, 0, 0, NULL);
    record.owner_create_result = owner;
    if(owner >= 0)
    {
        record.owner_start_result = sceKernelStartThread(owner, 0, NULL);
        if(record.owner_start_result >= 0)
        {
            SceUInt wait_us = GATE_THREAD_WAIT_US;
            int status = 0;
            record.owner_wait_result = sceKernelWaitThreadEnd(
                owner, &status, &wait_us);
        }
        record.owner_affinity_result = owner_affinity_result;
        record.owner_open_result = owner_open_result;
        record.owner_read_result = owner_read_result;
        record.owner_token = owner_handle.owner_token;
        record.owner_generation = owner_handle.generation;
        record.owner_value = owner_sample.value;
        record.owner_open_attempt_us = owner_open_attempt_us;

        /* Never let the main thread compete for the transport unless this
         * process positively observed the owner terminate.  Otherwise a
         * delayed worker could Open after a seemingly successful post Close
         * and turn normal app exit into an accidental process-exit test. */
        if(record.owner_wait_result >= 0)
        {
            struct vd_kernel_pmu_profiler_open_request request = make_request(
                VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
                VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
            struct vd_kernel_pmu_profiler_handle post_handle = {0};
            record.post_open_result = VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
            const uint64_t rearm_start_us = sceKernelGetProcessTimeWide();
            for(;;)
            {
                record.post_open_result = vdKernelPmuProfilerOpen(
                    &request, &post_handle);
                const uint64_t open_completed_us =
                    sceKernelGetProcessTimeWide();
                record.rearm_elapsed_us =
                    open_completed_us - rearm_start_us;
                if(record.post_open_result == 0)
                {
                    record.post_open_success_us = open_completed_us;
                    if(record.owner_open_attempt_us != 0 &&
                       open_completed_us >= record.owner_open_attempt_us)
                        record.lease_to_rearm_elapsed_us =
                            open_completed_us -
                                record.owner_open_attempt_us;
                }
                if(record.post_open_result !=
                   VD_KERNEL_ERROR_PMU_PROFILER_BUSY)
                    break;
                /* Keep servicing recovery past the two-second proof deadline
                 * so a failed gate does not return control while the original
                 * five-second lease may still be active.  A late acquisition
                 * is cleaned up below but can never set the PASS flag. */
                if(record.rearm_elapsed_us >= GATE_RECOVERY_CEILING_US)
                    break;
                sceKernelDelayThread(GATE_REARM_POLL_US);
            }

            struct vd_kernel_pmu_profiler_sample post_sample = {0};
            if(record.post_open_result == 0)
            {
                run_bounded_workload();
                record.post_read_result = vdKernelPmuProfilerRead(
                    &post_handle, &post_sample);
                record.post_close_result = vdKernelPmuProfilerClose(
                    &post_handle);
                record.post_owner_token = post_handle.owner_token;
                record.post_generation = post_handle.generation;
                record.post_value = post_sample.value;
            }
            if(record.post_open_result == 0 &&
               record.post_read_result == 0 &&
               sample_valid(&post_handle, &post_sample,
                            VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
               record.post_close_result == 0 &&
               record.rearm_elapsed_us <=
                   VD_PMU_THREAD_EXIT_REARM_DEADLINE_US &&
               record.owner_open_attempt_us != 0 &&
               record.post_open_success_us >=
                   record.owner_open_attempt_us &&
               record.lease_to_rearm_elapsed_us ==
                   record.post_open_success_us -
                       record.owner_open_attempt_us &&
               record.lease_to_rearm_elapsed_us <
                   VD_PMU_THREAD_EXIT_OWNER_LEASE_US &&
               (post_handle.owner_token != owner_handle.owner_token ||
                post_handle.generation != owner_handle.generation))
                record.flags |= VD_PMU_THREAD_EXIT_FLAG_REARM_SAMPLE;
        }
        record.owner_delete_result = sceKernelDeleteThread(owner);

        if(record.owner_affinity_result >= 0 &&
           record.owner_open_result == 0 &&
           record.owner_read_result == 0 &&
           sample_valid(&owner_handle, &owner_sample,
                        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
           record.owner_wait_result >= 0)
            record.flags |= VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE;
    }

    record.affinity_restore_result = affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), affinity_before);
    const int passed =
        (record.flags & (VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE |
                         VD_PMU_THREAD_EXIT_FLAG_REARM_SAMPLE)) ==
            (VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE |
             VD_PMU_THREAD_EXIT_FLAG_REARM_SAMPLE) &&
        record.owner_delete_result >= 0 &&
        record.affinity_restore_result >= 0;
    record.revision = 2;
    record.state = passed ? VD_PMU_THREAD_EXIT_COMPLETE :
                            VD_PMU_THREAD_EXIT_FAILED;
    if(passed)
        record.flags |= VD_PMU_THREAD_EXIT_FLAG_PASS;
    record.timestamp_us = sceKernelGetProcessTimeWide();
    const int journal = write_slot_verified(1, &record);

    psvDebugScreenPrintf(
        "owner open=%08X read=%08X wait=%08X delete=%08X\n",
        (uint32_t)record.owner_open_result,
        (uint32_t)record.owner_read_result,
        (uint32_t)record.owner_wait_result,
        (uint32_t)record.owner_delete_result);
    psvDebugScreenPrintf(
        "rearm open=%08X read=%08X close=%08X poll=%llu us\n",
        (uint32_t)record.post_open_result,
        (uint32_t)record.post_read_result,
        (uint32_t)record.post_close_result,
        (unsigned long long)record.rearm_elapsed_us);
    psvDebugScreenPrintf(
        "owner-open to rearm=%llu us (must be below %llu)\n",
        (unsigned long long)record.lease_to_rearm_elapsed_us,
        (unsigned long long)VD_PMU_THREAD_EXIT_OWNER_LEASE_US);
    psvDebugScreenPrintf(
        "affinity restore=%08X journal=%08X flags=%08X\n",
        (uint32_t)record.affinity_restore_result,
        (uint32_t)journal, record.flags);
    psvDebugScreenPrintf("Thread-exit gate: %s\n",
                         passed && journal == 0 ? "PASS" : "FAIL");
    const int quiescent = record.owner_wait_result >= 0 &&
                          record.owner_delete_result >= 0 &&
                          record.post_open_result == 0 &&
                          record.post_close_result == 0;
    if(quiescent && journal == 0)
        psvDebugScreenPrintf(
            "PMU quiescence proven; no process-exit lease was armed.\n"
            "Archive both records.\n");
    else if(quiescent)
        psvDebugScreenPrintf(
            "HARD STOP: PMU quiescent, but terminal journal verification "
            "failed.\n"
            "Do not close this app; preserve the storage failure at power "
            "off.\n");
    else
        psvDebugScreenPrintf(
            "HARD STOP: PMU quiescence unproven. Do not close this app.\n"
            "Power off fully and recover the plugin at the next boot.\n");
    return quiescent && journal == 0;
}

int main(void)
{
    if(psvDebugScreenInit() < 0)
        return 1;
    const int mkdir_result = sceIoMkdir("ux0:data/VitaDebugger", 0777);
    struct vd_pmu_thread_exit_record latest = {0};
    int latest_slot = -1;
    const int journal_result =
        mkdir_result < 0 && (uint32_t)mkdir_result != GATE_EEXIST ?
            mkdir_result : read_latest(&latest, &latest_slot);
    (void)latest_slot;
    struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    const int info_result = vdKernelPmuProfilerGetInfo(&info);

    psvDebugScreenPrintf("VitaDebugger PMU thread-exit gate\n\n");
    psvDebugScreenPrintf("Core 0 / lane 5 / no process-exit stage\n");
    psvDebugScreenPrintf("Opening this UI performs no PMU access.\n\n");
    psvDebugScreenPrintf("GetInfo: %08X\n", (uint32_t)info_result);
    if(info_result == 0)
        psvDebugScreenPrintf("ABI=%u caps=%08X lease=%u..%u ms\n",
                             info.abi_version, info.capabilities,
                             info.min_lease_ms, info.max_lease_ms);
    if(journal_result == VD_PMU_THREAD_EXIT_SELECT_EMPTY)
    {
        psvDebugScreenPrintf("Journal: empty; one-shot gate ready\n\n");
        psvDebugScreenPrintf(
            "X: run owner-thread exit and same-process re-arm\n");
    }
    else if(journal_result == 0)
    {
        psvDebugScreenPrintf(
            "Journal: LOCKED rev=%u state=%u flags=%08X\n",
            latest.revision, latest.state, latest.flags);
        psvDebugScreenPrintf(
            "Archive both slots; do not erase ambiguous evidence.\n");
    }
    else
        psvDebugScreenPrintf("Journal: LOCKED (%08X)\n",
                             (uint32_t)journal_result);
    psvDebugScreenPrintf("O: exit without running\n");

    SceCtrlData previous = {0};
    int ran = 0;
    int exit_allowed = 1;
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
        {
            if(!ran || exit_allowed)
                break;
            psvDebugScreenPrintf(
                "HARD STOP remains active; normal exit is disabled.\n");
        }
        if((pressed & SCE_CTRL_CROSS) == 0 || ran)
        {
            sceKernelDelayThread(20000);
            continue;
        }
        ran = 1;
        if(journal_result == VD_PMU_THREAD_EXIT_SELECT_EMPTY)
            exit_allowed = run_gate(info_result, &info);
        else
            psvDebugScreenPrintf(
                "LOCKED: journal is not an empty runnable state.\n");
        if(exit_allowed)
            psvDebugScreenPrintf("O: exit\n");
        else
            psvDebugScreenPrintf("Normal exit disabled by HARD STOP.\n");
    }
    (void)psvDebugScreenFinish();
    return 0;
}
