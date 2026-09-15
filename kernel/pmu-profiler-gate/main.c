#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>

#include "debugScreen.h"
#include "journal.h"
#include "vitadebug_pmu_profiler.h"

#if !defined(VD_PMU_PROFILER_GATE_EVENT_CODE)
#error "the disposable gate requires one fixed event code"
#endif

#if VD_PMU_PROFILER_GATE_EVENT_CODE != 0x01 && \
    VD_PMU_PROFILER_GATE_EVENT_CODE != 0x03 && \
    VD_PMU_PROFILER_GATE_EVENT_CODE != 0x10
#error "the disposable gate admits only 0x01, 0x03, or 0x10"
#endif

#define GATE_CORE_AFFINITY 0x00010000
#define GATE_WORK_WORDS 8192u
#define GATE_WORK_ROUNDS 32u
#define GATE_ENOENT UINT32_C(0x80010002)
#define GATE_EEXIST UINT32_C(0x80010011)

static volatile uint32_t gate_work[GATE_WORK_WORDS];
static volatile uint32_t gate_sink;

static const char* record_path(int slot)
{
    return slot == 0 ? VD_PMU_PROFILER_GATE_RECORD_A_PATH :
                       VD_PMU_PROFILER_GATE_RECORD_B_PATH;
}

static int bytes_equal(const void* left, const void* right, size_t size)
{
    const uint8_t* lhs = (const uint8_t*)left;
    const uint8_t* rhs = (const uint8_t*)right;
    for(size_t i = 0; i < size; ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static int inspect_slot(
    int slot, struct vd_pmu_profiler_gate_record* record,
    int* present, int* valid)
{
    SceIoStat stat;
    *present = 0;
    *valid = 0;
    int result = sceIoGetstat(record_path(slot), &stat);
    if(result < 0)
        return (uint32_t)result == GATE_ENOENT ? 0 : result;
    *present = 1;
    if(stat.st_size != (SceOff)sizeof(*record))
        return 0;
    SceUID fd = sceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    if(fd < 0)
        return fd;
    result = sceIoRead(fd, record, sizeof(*record));
    const int close_result = sceIoClose(fd);
    if(result != (int)sizeof(*record))
        return result < 0 ? result : -1;
    if(close_result < 0)
        return close_result;
    *valid = vdPmuProfilerGateRecordValid(record);
    return 0;
}

static int read_latest(
    struct vd_pmu_profiler_gate_record* latest, int* latest_slot)
{
    struct vd_pmu_profiler_gate_record a = {0};
    struct vd_pmu_profiler_gate_record b = {0};
    int present_a = 0;
    int present_b = 0;
    int valid_a = 0;
    int valid_b = 0;
    const int result_a = inspect_slot(0, &a, &present_a, &valid_a);
    const int result_b = inspect_slot(1, &b, &present_b, &valid_b);
    if(result_a < 0)
        return result_a;
    if(result_b < 0)
        return result_b;
    const int selected = vdPmuProfilerGateSelectLatest(
        present_a, valid_a, &a, present_b, valid_b, &b);
    if(selected < 0)
        return selected;
    *latest = selected == 0 ? a : b;
    *latest_slot = selected;
    return 0;
}

static int write_slot_verified(
    int slot, struct vd_pmu_profiler_gate_record* record)
{
    struct vd_pmu_profiler_gate_record verification = {0};
    record->magic = VD_PMU_PROFILER_GATE_RECORD_MAGIC;
    record->version = VD_PMU_PROFILER_GATE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuProfilerGateChecksum(record);

    SceUID fd = sceIoOpen(record_path(slot),
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

static const char* event_name(void)
{
    switch(VD_PMU_PROFILER_GATE_EVENT_CODE)
    {
        case 0x01: return "L1 instruction-cache miss/refill";
        case 0x03: return "L1 data-cache miss/refill";
        case 0x10: return "branch mispredict";
        default: return "invalid";
    }
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

static void print_intro(
    int info_result, const struct vd_kernel_pmu_profiler_info* info,
    int journal_result,
    const struct vd_pmu_profiler_gate_record* latest)
{
    psvDebugScreenPrintf("VitaDebugger PMU profiler transport gate\n\n");
    psvDebugScreenPrintf("Fixed core 0 / physical lane 5 / event 0x%02X\n",
                         VD_PMU_PROFILER_GATE_EVENT_CODE);
    psvDebugScreenPrintf("%s\n", event_name());
    psvDebugScreenPrintf("One real-event attempt is allowed per reboot.\n");
    psvDebugScreenPrintf("Opening this UI performs no PMU register access.\n\n");
    psvDebugScreenPrintf("GetInfo: 0x%08X\n", (uint32_t)info_result);
    if(info_result == 0)
    {
        psvDebugScreenPrintf(
            "ABI=%u caps=%08X core=%u lane=%u lease=%u..%u ms\n",
            info->abi_version, info->capabilities, info->fixed_core,
            info->fixed_counter, info->min_lease_ms,
            info->max_lease_ms);
    }
    if(journal_result == VD_PMU_PROFILER_GATE_SELECT_EMPTY)
        psvDebugScreenPrintf("Journal: empty and ready\n");
    else if(journal_result == 0 && latest)
        psvDebugScreenPrintf(
            "Journal: rev=%u state=%u event=%02X flags=%08X\n",
            latest->revision, latest->state, latest->event_code,
            latest->flags);
    else
        psvDebugScreenPrintf("Journal: LOCKED (%08X)\n",
                             (uint32_t)journal_result);
    if(journal_result == VD_PMU_PROFILER_GATE_SELECT_EMPTY)
        psvDebugScreenPrintf("\nX: run the single bounded attempt\n");
    else
        psvDebugScreenPrintf(
            "Archive and remove both journal slots before another run.\n");
    psvDebugScreenPrintf("O: exit without running\n");
}

int main(void)
{
    SceCtrlData previous = {0};
    int ran = 0;
    if(psvDebugScreenInit() < 0)
        return 1;

    struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    const int info_result = vdKernelPmuProfilerGetInfo(&info);
    struct vd_pmu_profiler_gate_record latest = {0};
    int latest_slot = -1;
    int journal_result;
    const int mkdir_result = sceIoMkdir("ux0:data/VitaDebugger", 0777);
    if(mkdir_result < 0 && (uint32_t)mkdir_result != GATE_EEXIST)
        journal_result = mkdir_result;
    else
        journal_result = read_latest(&latest, &latest_slot);
    print_intro(info_result, &info, journal_result,
                journal_result == 0 ? &latest : NULL);

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
        if((pressed & SCE_CTRL_CROSS) == 0 || ran ||
           journal_result != VD_PMU_PROFILER_GATE_SELECT_EMPTY)
        {
            sceKernelDelayThread(20000);
            continue;
        }
        ran = 1;
        psvDebugScreenPrintf("\nRunning bounded event 0x%02X...\n",
                             VD_PMU_PROFILER_GATE_EVENT_CODE);

        int affinity_before = sceKernelGetThreadCpuAffinityMask(
            sceKernelGetThreadId());
        int affinity_result = affinity_before < 0 ? affinity_before :
            sceKernelChangeThreadCpuAffinityMask(
                sceKernelGetThreadId(), GATE_CORE_AFFINITY);

        struct vd_pmu_profiler_gate_record record = {
            .revision = 1,
            .state = VD_PMU_PROFILER_GATE_ATTEMPTED,
            .event_code = VD_PMU_PROFILER_GATE_EVENT_CODE,
            .timestamp_us = sceKernelGetProcessTimeWide(),
            .info_result = info_result,
            .affinity_result = affinity_result,
            .open_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN,
            .read_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN,
            .close_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN,
            .affinity_restore_result =
                VD_PMU_PROFILER_GATE_RESULT_NOT_RUN,
        };
        int attempted_journal_result = -1;
        if(info_result == 0 && affinity_result >= 0 &&
           info.fixed_core == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
           info.fixed_counter == VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
           (info.capabilities & VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS) != 0 &&
           info_has_event(&info, VD_PMU_PROFILER_GATE_EVENT_CODE))
            attempted_journal_result = write_slot_verified(0, &record);
        if(attempted_journal_result < 0)
        {
            int affinity_restore = affinity_before < 0 ? affinity_before :
                sceKernelChangeThreadCpuAffinityMask(
                    sceKernelGetThreadId(), affinity_before);
            psvDebugScreenPrintf(
                "attempt journal=%08X affinity restore=%08X\n",
                (uint32_t)attempted_journal_result,
                (uint32_t)affinity_restore);
            psvDebugScreenPrintf(
                "BLOCKED before PMU open; no counter mutation attempted.\n");
            psvDebugScreenPrintf("O: exit\n");
            continue;
        }
        psvDebugScreenPrintf(
            "Attempt journal is synced and read back; entering kernel.\n");

        struct vd_kernel_pmu_profiler_open_request request = {
            .struct_size = sizeof(request),
            .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
            .event_code = VD_PMU_PROFILER_GATE_EVENT_CODE,
            .lease_ms = VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS,
            .flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT,
        };
        struct vd_kernel_pmu_profiler_handle handle = {0};
        struct vd_kernel_pmu_profiler_sample sample = {0};
        int open_result = VD_KERNEL_ERROR_PMU_PROFILER_STATE;
        int read_result = VD_KERNEL_ERROR_PMU_PROFILER_STATE;
        int close_result = VD_KERNEL_ERROR_PMU_PROFILER_STATE;
        if(attempted_journal_result == 0)
        {
            open_result = vdKernelPmuProfilerOpen(&request, &handle);
            if(open_result == 0)
            {
                run_bounded_workload();
                read_result = vdKernelPmuProfilerRead(&handle, &sample);
                /* Always request exact restoration before doing display I/O. */
                close_result = vdKernelPmuProfilerClose(&handle);
            }
        }
        int affinity_restore = affinity_before < 0 ? affinity_before :
            sceKernelChangeThreadCpuAffinityMask(
                sceKernelGetThreadId(), affinity_before);

        const int sample_valid = read_result == 0 &&
            sample.struct_size == sizeof(sample) &&
            sample.abi_version == VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
            sample.owner_token == handle.owner_token &&
            sample.generation == handle.generation &&
            sample.event_code == VD_PMU_PROFILER_GATE_EVENT_CODE &&
            sample.core_id == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
            sample.physical_counter ==
                VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
            sample.flags == 0 && sample.reserved[0] == 0 &&
            sample.reserved[1] == 0;

        record.revision = 2;
        record.timestamp_us = sceKernelGetProcessTimeWide();
        record.open_result = open_result;
        record.read_result = read_result;
        record.close_result = close_result;
        record.affinity_restore_result = affinity_restore;
        record.owner_token = handle.owner_token;
        record.generation = handle.generation;
        record.value_low = (uint32_t)sample.value;
        record.value_high = (uint32_t)(sample.value >> 32);
        record.core_id = sample_valid ? sample.core_id : 0;
        record.physical_counter = sample_valid ?
            sample.physical_counter : 0;
        const int gate_pass = info_result == 0 && affinity_result >= 0 &&
            affinity_restore >= 0 && open_result == 0 && sample_valid &&
            close_result == 0;
        if(open_result == 0 && close_result == 0)
        {
            record.state = VD_PMU_PROFILER_GATE_COMPLETE;
            record.flags = VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN;
            if(sample_valid)
                record.flags |= VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID;
            if(gate_pass)
                record.flags |= VD_PMU_PROFILER_GATE_FLAG_PASS;
        }
        else
        {
            record.state = VD_PMU_PROFILER_GATE_RESTORE_REQUIRED;
            record.flags = 0;
        }
        const int completion_journal_result =
            write_slot_verified(1, &record);
        psvDebugScreenPrintf(
            "affinity=%08X open=%08X read=%08X close=%08X restore=%08X\n",
            (uint32_t)affinity_result, (uint32_t)open_result,
            (uint32_t)read_result, (uint32_t)close_result,
            (uint32_t)affinity_restore);
        psvDebugScreenPrintf("counter delta=%llu (zero is allowed)\n",
                             (unsigned long long)sample.value);
        psvDebugScreenPrintf("Exact close/restoration: %s\n",
                             close_result == 0 ? "PROVEN" : "NOT PROVEN");
        psvDebugScreenPrintf("completion journal=%08X\n",
                             (uint32_t)completion_journal_result);
        psvDebugScreenPrintf("Gate result: %s\n",
            gate_pass && completion_journal_result == 0 ? "PASS" : "FAIL");
        psvDebugScreenPrintf(
            "Do not run another real event until after a full reboot.\n");
        psvDebugScreenPrintf("O: exit\n");
    }
    (void)psvDebugScreenFinish();
    return 0;
}
