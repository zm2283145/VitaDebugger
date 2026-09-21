#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdint.h>

#include "debugScreen.h"
#include "journal.h"
#include "vitadebug_pmu_profiler.h"
#include "vitaprofiler_tcp_vita.h"

#ifndef VD_PMU_CLEANUP_GATE_STAGE
#error "VD_PMU_CLEANUP_GATE_STAGE must select one cleanup stage"
#endif
#if VD_PMU_CLEANUP_GATE_STAGE < 1 || VD_PMU_CLEANUP_GATE_STAGE > 5
#error "VD_PMU_CLEANUP_GATE_STAGE is outside the reviewed stage set"
#endif

#ifndef VD_PMU_CLEANUP_HOST_A
#define VD_PMU_CLEANUP_HOST_A 127
#endif
#ifndef VD_PMU_CLEANUP_HOST_B
#define VD_PMU_CLEANUP_HOST_B 0
#endif
#ifndef VD_PMU_CLEANUP_HOST_C
#define VD_PMU_CLEANUP_HOST_C 0
#endif
#ifndef VD_PMU_CLEANUP_HOST_D
#define VD_PMU_CLEANUP_HOST_D 1
#endif
#ifndef VD_PMU_CLEANUP_PORT
#define VD_PMU_CLEANUP_PORT 18196
#endif

#define GATE_CORE_AFFINITY 0x00010000
#define GATE_THREAD_STACK (16u * 1024u)
#define GATE_WAIT_US 2000000u
#define GATE_TIMEOUT_WAIT_US 600000u
#define GATE_REARM_DEADLINE_US UINT64_C(2000000)
#define GATE_KILL_ARM_WINDOW_US UINT64_C(4000000)
#define GATE_REARM_POLL_US 20000u
#define GATE_WORK_WORDS 4096u
#define GATE_WORK_ROUNDS 24u
#define GATE_NET_MEMORY_BYTES (1024u * 1024u)
#define GATE_ENOENT UINT32_C(0x80010002)
#define GATE_EEXIST UINT32_C(0x80010011)

enum gate_result_index {
    GATE_RESULT_OPEN = 0,
    GATE_RESULT_READ,
    GATE_RESULT_CLOSE,
    GATE_RESULT_AUX_CREATE,
    GATE_RESULT_AUX_START,
    GATE_RESULT_AUX_WAIT,
    GATE_RESULT_AUX_DELETE,
    GATE_RESULT_AUX_ACTION,
    GATE_RESULT_AUX_CLEANUP,
    GATE_RESULT_REARM_OPEN,
    GATE_RESULT_REARM_READ,
    GATE_RESULT_REARM_CLOSE,
    GATE_RESULT_AFFINITY,
    GATE_RESULT_AFFINITY_RESTORE,
    GATE_RESULT_NET_START,
    GATE_RESULT_NET_CONNECT,
    GATE_RESULT_NET_PRELUDE,
    GATE_RESULT_NET_FAILURE,
    GATE_RESULT_NET_CLOSE,
    GATE_RESULT_NET_STOP,
    GATE_RESULT_EXIT_RETURN,
    GATE_RESULT_POST_DISCONNECT_READ,
};

static volatile uint32_t gate_work[GATE_WORK_WORDS];
static volatile uint32_t gate_sink_value;
static volatile int32_t contender_result;
static volatile int32_t contender_cleanup_result;
static uint8_t gate_net_memory[GATE_NET_MEMORY_BYTES]
    __attribute__((aligned(64)));
static struct vp_vita_tcp_sce_net_backend gate_tcp_backend =
    VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
static struct vp_vita_tcp_sink gate_tcp_sink;
static int gate_net_module_loaded;
static int gate_net_initialized;
static int gate_netctl_initialized;
static int gate_tcp_initialized;

static const char* const record_paths[5][VD_PMU_CLEANUP_SLOT_COUNT] = {
    {
        "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-a.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-b.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-c.bin",
    },
    {
        "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-a.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-b.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-timeout-c.bin",
    },
    {
        "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-a.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-b.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-disconnect-c.bin",
    },
    {
        "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-a.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-b.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-normal-exit-c.bin",
    },
    {
        "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-a.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-b.bin",
        "ux0:data/VitaDebugger/pmu-cleanup-v2-abrupt-exit-c.bin",
    },
};

static const char* record_path(size_t slot)
{
    return record_paths[VD_PMU_CLEANUP_GATE_STAGE - 1][slot];
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

static void initialize_record(struct vd_pmu_cleanup_record* record)
{
    uint8_t* bytes = (uint8_t*)record;
    for(size_t i = 0; i < sizeof(*record); ++i)
        bytes[i] = 0;
    for(size_t i = 0;
        i < sizeof(record->results) / sizeof(record->results[0]); ++i)
        record->results[i] = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    record->restored_status_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    record->final_status_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
}

static int inspect_slot(
    size_t slot, struct vd_pmu_cleanup_record* record,
    int* present, int* valid)
{
    if(slot >= VD_PMU_CLEANUP_SLOT_COUNT)
        return -1;
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
    *valid = vdPmuCleanupRecordValid(record);
    return 0;
}

static int read_latest(
    struct vd_pmu_cleanup_record* latest, int* latest_slot)
{
    struct vd_pmu_cleanup_record records[VD_PMU_CLEANUP_SLOT_COUNT];
    int present[VD_PMU_CLEANUP_SLOT_COUNT] = {0};
    int valid[VD_PMU_CLEANUP_SLOT_COUNT] = {0};
    for(size_t i = 0; i < VD_PMU_CLEANUP_SLOT_COUNT; ++i)
    {
        const int result = inspect_slot(
            i, &records[i], &present[i], &valid[i]);
        if(result < 0)
            return result;
    }
    const int selected = vdPmuCleanupSelectLatest(
        present, valid, records, VD_PMU_CLEANUP_SLOT_COUNT);
    if(selected < 0)
        return selected;
    *latest = records[selected];
    *latest_slot = selected;
    return 0;
}

static int write_slot_verified(
    size_t slot, struct vd_pmu_cleanup_record* record)
{
    struct vd_pmu_cleanup_record verification;
    if(slot >= VD_PMU_CLEANUP_SLOT_COUNT)
        return -1;
    record->magic = VD_PMU_CLEANUP_RECORD_MAGIC;
    record->version = VD_PMU_CLEANUP_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuCleanupChecksum(record);
    SceUID fd = sceIoOpen(
        record_path(slot),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
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
    return result < 0 || !present || !valid ||
           !bytes_equal(&verification, record, sizeof(verification)) ?
        (result < 0 ? result : -1) : 0;
}

static struct vd_kernel_pmu_profiler_open_request request_for(
    uint32_t event, uint32_t lease_ms)
{
    const struct vd_kernel_pmu_profiler_open_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
        .event_code = event,
        .lease_ms = lease_ms,
        .flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT,
    };
    return request;
}

static int get_status(struct vd_kernel_pmu_profiler_status* status)
{
    *status = (struct vd_kernel_pmu_profiler_status){
        .struct_size = sizeof(*status),
        .abi_version = VD_KERNEL_PMU_PROFILER_STATUS_ABI_VERSION,
    };
    return vdKernelPmuProfilerGetStatus(status);
}

static int sample_valid(
    const struct vd_kernel_pmu_profiler_handle* handle,
    const struct vd_kernel_pmu_profiler_sample* sample,
    uint32_t event)
{
    return handle && sample &&
        sample->struct_size == sizeof(*sample) &&
        sample->abi_version == VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
        sample->owner_token == handle->owner_token &&
        sample->generation == handle->generation &&
        sample->event_code == event &&
        sample->core_id == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
        sample->physical_counter ==
            VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
}

__attribute__((noinline))
static void run_workload(void)
{
    uint32_t value = UINT32_C(0x2468ace1);
    for(uint32_t round = 0; round < GATE_WORK_ROUNDS; ++round)
        for(uint32_t i = 0; i < GATE_WORK_WORDS; i += 17u)
        {
            value ^= gate_work[
                (i + round * 31u) & (GATE_WORK_WORDS - 1u)];
            value = (value << 5) | (value >> 27);
        }
    gate_sink_value = value;
}

static int open_read_close(
    struct vd_pmu_cleanup_record* record, size_t handle_index,
    size_t open_result_index, uint32_t event)
{
    struct vd_kernel_pmu_profiler_open_request request =
        request_for(event, VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    struct vd_kernel_pmu_profiler_handle* handle =
        &record->handles[handle_index];
    struct vd_kernel_pmu_profiler_sample* sample =
        &record->samples[handle_index];
    record->results[open_result_index] =
        vdKernelPmuProfilerOpen(&request, handle);
    if(record->results[open_result_index] != 0)
        return 0;
    run_workload();
    record->results[open_result_index + 1] =
        vdKernelPmuProfilerRead(handle, sample);
    record->results[open_result_index + 2] =
        vdKernelPmuProfilerClose(handle);
    return record->results[open_result_index + 1] == 0 &&
        sample_valid(handle, sample, event) &&
        record->results[open_result_index + 2] == 0;
}

static int contender_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    struct vd_kernel_pmu_profiler_open_request request =
        request_for(VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
                    VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
    struct vd_kernel_pmu_profiler_handle handle = {0};
    contender_cleanup_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    contender_result = vdKernelPmuProfilerOpen(&request, &handle);
    if(contender_result == 0)
        contender_cleanup_result = vdKernelPmuProfilerClose(&handle);
    return 0;
}

static int run_conflict(struct vd_pmu_cleanup_record* record)
{
    struct vd_kernel_pmu_profiler_open_request request =
        request_for(VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
                    VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    record->results[GATE_RESULT_OPEN] = vdKernelPmuProfilerOpen(
        &request, &record->handles[0]);
    if(record->results[GATE_RESULT_OPEN] != 0)
        return 0;
    run_workload();
    record->results[GATE_RESULT_READ] = vdKernelPmuProfilerRead(
        &record->handles[0], &record->samples[0]);
    contender_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    contender_cleanup_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    SceUID contender = sceKernelCreateThread(
        "vd pmu cleanup contender", contender_main, 0x40,
        GATE_THREAD_STACK, 0, 0, NULL);
    record->results[GATE_RESULT_AUX_CREATE] = contender;
    if(contender >= 0)
    {
        record->results[GATE_RESULT_AUX_START] =
            sceKernelStartThread(contender, 0, NULL);
        if(record->results[GATE_RESULT_AUX_START] >= 0)
        {
            SceUInt wait_us = GATE_WAIT_US;
            int status = 0;
            record->results[GATE_RESULT_AUX_WAIT] =
                sceKernelWaitThreadEnd(
                    contender, &status, &wait_us);
        }
        record->results[GATE_RESULT_AUX_ACTION] = contender_result;
        record->results[GATE_RESULT_AUX_CLEANUP] =
            contender_cleanup_result;
        record->results[GATE_RESULT_AUX_DELETE] =
            sceKernelDeleteThread(contender);
    }
    record->results[GATE_RESULT_CLOSE] =
        vdKernelPmuProfilerClose(&record->handles[0]);
    return record->results[GATE_RESULT_READ] == 0 &&
        sample_valid(&record->handles[0], &record->samples[0],
                     VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
        record->results[GATE_RESULT_AUX_CREATE] >= 0 &&
        record->results[GATE_RESULT_AUX_START] >= 0 &&
        record->results[GATE_RESULT_AUX_WAIT] >= 0 &&
        record->results[GATE_RESULT_AUX_DELETE] >= 0 &&
        record->results[GATE_RESULT_AUX_ACTION] ==
            VD_KERNEL_ERROR_PMU_PROFILER_BUSY &&
        record->results[GATE_RESULT_AUX_CLEANUP] ==
            VD_PMU_CLEANUP_RESULT_NOT_RUN &&
        record->results[GATE_RESULT_CLOSE] == 0;
}

static int run_timeout(struct vd_pmu_cleanup_record* record)
{
    struct vd_kernel_pmu_profiler_open_request request =
        request_for(VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS,
                    VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS);
    record->results[GATE_RESULT_OPEN] = vdKernelPmuProfilerOpen(
        &request, &record->handles[0]);
    if(record->results[GATE_RESULT_OPEN] != 0)
        return 0;
    run_workload();
    record->results[GATE_RESULT_READ] = vdKernelPmuProfilerRead(
        &record->handles[0], &record->samples[0]);
    sceKernelDelayThread(GATE_TIMEOUT_WAIT_US);
    record->results[GATE_RESULT_AUX_ACTION] =
        vdKernelPmuProfilerRead(
            &record->handles[0], &record->samples[1]);
    record->results[GATE_RESULT_CLOSE] =
        vdKernelPmuProfilerClose(&record->handles[0]);
    return record->results[GATE_RESULT_READ] == 0 &&
        sample_valid(&record->handles[0], &record->samples[0],
                     VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS) &&
        record->results[GATE_RESULT_AUX_ACTION] ==
            VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
        record->results[GATE_RESULT_CLOSE] == 0;
}

static int start_network(void)
{
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if(result < 0)
        return result;
    gate_net_module_loaded = 1;
    SceNetInitParam init = {
        .memory = gate_net_memory,
        .size = (int)sizeof(gate_net_memory),
    };
    result = sceNetInit(&init);
    if(result < 0)
        return result;
    gate_net_initialized = 1;
    result = sceNetCtlInit();
    if(result < 0)
        return result;
    gate_netctl_initialized = 1;
    for(uint32_t attempt = 0; attempt < 100u; ++attempt)
    {
        int state = SCE_NETCTL_STATE_DISCONNECTED;
        result = sceNetCtlInetGetState(&state);
        if(result < 0)
            return result;
        if(state == SCE_NETCTL_STATE_CONNECTED)
            return 0;
        sceKernelDelayThread(100000);
    }
    return -1;
}

static int stop_network(void)
{
    int first_error = 0;
    if(gate_tcp_initialized)
    {
        int result = VP_ERROR_IO;
        for(uint32_t attempt = 0;
            attempt < 4u && result != VP_RESULT_OK; ++attempt)
            result = vp_vita_tcp_sink_close(&gate_tcp_sink);
        if(result != VP_RESULT_OK)
            return result;
        gate_tcp_initialized = 0;
    }
    if(gate_netctl_initialized)
    {
        sceNetCtlTerm();
        gate_netctl_initialized = 0;
    }
    if(gate_net_initialized)
    {
        const int result = sceNetTerm();
        if(result < 0 && first_error == 0)
            first_error = result;
        else if(result >= 0)
            gate_net_initialized = 0;
    }
    if(gate_net_module_loaded && !gate_net_initialized)
    {
        const int result = sceSysmoduleUnloadModule(
            SCE_SYSMODULE_NET);
        if(result < 0 && first_error == 0)
            first_error = result;
        else if(result >= 0)
            gate_net_module_loaded = 0;
    }
    return first_error;
}

static int run_disconnect(struct vd_pmu_cleanup_record* record)
{
    record->results[GATE_RESULT_NET_START] = start_network();
    if(record->results[GATE_RESULT_NET_START] != 0)
    {
        record->results[GATE_RESULT_NET_STOP] = stop_network();
        return 0;
    }
    struct vp_vita_tcp_sink_config config;
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = VD_PMU_CLEANUP_HOST_A;
    config.endpoint.ipv4[1] = VD_PMU_CLEANUP_HOST_B;
    config.endpoint.ipv4[2] = VD_PMU_CLEANUP_HOST_C;
    config.endpoint.ipv4[3] = VD_PMU_CLEANUP_HOST_D;
    config.endpoint.port = VD_PMU_CLEANUP_PORT;
    int result = vp_vita_tcp_sce_net_ops_init(
        &gate_tcp_backend, &config.ops);
    if(result == VP_RESULT_OK)
    {
        config.ops_user = &gate_tcp_backend;
        result = vp_vita_tcp_sink_init(&gate_tcp_sink, &config);
    }
    if(result == VP_RESULT_OK)
    {
        gate_tcp_initialized = 1;
        result = vp_vita_tcp_sink_connect(&gate_tcp_sink);
    }
    record->results[GATE_RESULT_NET_CONNECT] = result;
    if(result != VP_RESULT_OK)
    {
        record->results[GATE_RESULT_NET_STOP] = stop_network();
        return 0;
    }
    static const uint8_t prelude[] = "VDPMU-DROP-V1\n";
    record->results[GATE_RESULT_NET_PRELUDE] =
        vp_vita_tcp_sink_write(
            &gate_tcp_sink, prelude, sizeof(prelude) - 1u);
    if(record->results[GATE_RESULT_NET_PRELUDE] != VP_RESULT_OK)
    {
        record->results[GATE_RESULT_NET_STOP] = stop_network();
        return 0;
    }
    struct vd_kernel_pmu_profiler_open_request request =
        request_for(VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
                    VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
    record->results[GATE_RESULT_OPEN] = vdKernelPmuProfilerOpen(
        &request, &record->handles[0]);
    if(record->results[GATE_RESULT_OPEN] == 0)
    {
        run_workload();
        record->results[GATE_RESULT_READ] = vdKernelPmuProfilerRead(
            &record->handles[0], &record->samples[0]);
        sceKernelDelayThread(100000);
        static const uint8_t payload[4096] = {0xa5};
        record->results[GATE_RESULT_NET_FAILURE] = VP_RESULT_OK;
        for(uint32_t attempt = 0;
            attempt < 8u &&
            record->results[GATE_RESULT_NET_FAILURE] == VP_RESULT_OK;
            ++attempt)
            record->results[GATE_RESULT_NET_FAILURE] =
                vp_vita_tcp_sink_write(
                    &gate_tcp_sink, payload, sizeof(payload));
        record->results[GATE_RESULT_POST_DISCONNECT_READ] =
            vdKernelPmuProfilerRead(
                &record->handles[0], &record->samples[2]);
        record->results[GATE_RESULT_CLOSE] =
            vdKernelPmuProfilerClose(&record->handles[0]);
    }
    record->results[GATE_RESULT_NET_CLOSE] =
        vp_vita_tcp_sink_close(&gate_tcp_sink);
    if(record->results[GATE_RESULT_NET_CLOSE] == VP_RESULT_OK)
        gate_tcp_initialized = 0;
    record->results[GATE_RESULT_NET_STOP] = stop_network();
    return record->results[GATE_RESULT_OPEN] == 0 &&
        record->results[GATE_RESULT_READ] == 0 &&
        sample_valid(&record->handles[0], &record->samples[0],
                     VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
        record->results[GATE_RESULT_NET_FAILURE] == VP_ERROR_IO &&
        record->results[GATE_RESULT_POST_DISCONNECT_READ] == 0 &&
        sample_valid(&record->handles[0], &record->samples[2],
                     VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
        record->results[GATE_RESULT_CLOSE] == 0 &&
        record->results[GATE_RESULT_NET_CLOSE] == VP_RESULT_OK &&
        record->results[GATE_RESULT_NET_STOP] == 0;
}

static int run_rearm(struct vd_pmu_cleanup_record* record)
{
    const uint64_t started = sceKernelGetProcessTimeWide();
    const int passed = open_read_close(
        record, 1, GATE_RESULT_REARM_OPEN,
        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS);
    record->rearm_elapsed_us =
        sceKernelGetProcessTimeWide() - started;
    return passed &&
        record->rearm_elapsed_us <= GATE_REARM_DEADLINE_US;
}

static int finish_stage(
    struct vd_pmu_cleanup_record* record, size_t slot,
    int action_passed, int affinity_before)
{
    record->restored_status_result = get_status(&record->restored);
    const int restored = record->restored_status_result == 0 &&
        vdPmuCleanupStatusIdle(&record->restored) &&
        vdPmuCleanupSnapshotEqual(
            &record->baseline.snapshot, &record->restored.snapshot);
    if(record->stage == VD_PMU_CLEANUP_STAGE_NORMAL_EXIT &&
       (record->restored.active_process_normal_exit_cleanup_count <=
            record->baseline.active_process_normal_exit_cleanup_count ||
        record->restored.active_process_kill_cleanup_count !=
            record->baseline.active_process_kill_cleanup_count))
        action_passed = 0;
    if(record->stage == VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT &&
       (record->restored.active_process_kill_cleanup_count <=
            record->baseline.active_process_kill_cleanup_count ||
        record->restored.active_process_normal_exit_cleanup_count !=
            record->baseline.active_process_normal_exit_cleanup_count))
        action_passed = 0;
    int rearmed = action_passed && restored &&
        run_rearm(record);
    record->results[GATE_RESULT_AFFINITY_RESTORE] =
        affinity_before < 0 ? affinity_before :
        sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), affinity_before);
    if(record->results[GATE_RESULT_AFFINITY_RESTORE] < 0)
        rearmed = 0;
    record->final_status_result = get_status(&record->final);
    const int final = record->final_status_result == 0 &&
        vdPmuCleanupStatusIdle(&record->final) &&
        vdPmuCleanupSnapshotEqual(
            &record->baseline.snapshot, &record->final.snapshot);
    if(action_passed)
        record->flags |= VD_PMU_CLEANUP_FLAG_ACTION_PROVEN;
    if(restored)
        record->flags |= VD_PMU_CLEANUP_FLAG_RESTORED;
    if(rearmed)
        record->flags |= VD_PMU_CLEANUP_FLAG_REARMED;
    const int passed = record->flags ==
            (VD_PMU_CLEANUP_FLAG_ACTION_PROVEN |
             VD_PMU_CLEANUP_FLAG_RESTORED |
             VD_PMU_CLEANUP_FLAG_REARMED) &&
        final &&
        record->final.rearm_count > record->baseline.rearm_count;
    record->revision = slot == 2 ? 3 : 2;
    record->state = passed ? VD_PMU_CLEANUP_COMPLETE :
        VD_PMU_CLEANUP_FAILED;
    if(passed)
        record->flags |= VD_PMU_CLEANUP_FLAG_PASS;
    record->finished_us = sceKernelGetProcessTimeWide();
    return write_slot_verified(slot, record) == 0 && passed;
}

static int info_ready(
    int result, const struct vd_kernel_pmu_profiler_info* info)
{
    return result == 0 && info &&
        (info->capabilities & VD_PMU_CLEANUP_REQUIRED_CAPABILITIES) ==
            VD_PMU_CLEANUP_REQUIRED_CAPABILITIES &&
        (info->capabilities &
         VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT) == 0 &&
        info->fixed_core == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
        info->fixed_counter == VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
}

static int run_initial(
    int info_result, const struct vd_kernel_pmu_profiler_info* info)
{
    struct vd_pmu_cleanup_record record;
    initialize_record(&record);
    record.revision = 1;
    record.state = VD_PMU_CLEANUP_ATTEMPTED;
    record.stage = VD_PMU_CLEANUP_GATE_STAGE;
    record.started_us = sceKernelGetProcessTimeWide();
    record.info_result = info_result;
    record.capabilities = info_result == 0 ? info->capabilities : 0;
    const int affinity_before =
        sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    record.results[GATE_RESULT_AFFINITY] = affinity_before < 0 ?
        affinity_before : sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    record.baseline_status_result = get_status(&record.baseline);
    if(!info_ready(info_result, info) ||
       record.results[GATE_RESULT_AFFINITY] < 0 ||
       record.baseline_status_result != 0 ||
       !vdPmuCleanupStatusIdle(&record.baseline) ||
       write_slot_verified(0, &record) < 0)
    {
        psvDebugScreenPrintf(
            "BLOCKED before PMU mutation: prerequisites/journal.\n");
        return 0;
    }

    if(VD_PMU_CLEANUP_GATE_STAGE ==
           VD_PMU_CLEANUP_STAGE_NORMAL_EXIT ||
       VD_PMU_CLEANUP_GATE_STAGE ==
           VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT)
    {
        struct vd_kernel_pmu_profiler_open_request request =
            request_for(
                VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
                VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS);
        record.results[GATE_RESULT_OPEN] =
            vdKernelPmuProfilerOpen(&request, &record.handles[0]);
        if(record.results[GATE_RESULT_OPEN] == 0)
        {
            run_workload();
            record.results[GATE_RESULT_READ] =
                vdKernelPmuProfilerRead(
                    &record.handles[0], &record.samples[0]);
        }
        if(record.results[GATE_RESULT_OPEN] != 0 ||
           record.results[GATE_RESULT_READ] != 0 ||
           !sample_valid(
               &record.handles[0], &record.samples[0],
               VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT))
        {
            if(record.results[GATE_RESULT_OPEN] == 0)
                record.results[GATE_RESULT_CLOSE] =
                    vdKernelPmuProfilerClose(&record.handles[0]);
            record.revision = 2;
            record.state = VD_PMU_CLEANUP_FAILED;
            record.finished_us = sceKernelGetProcessTimeWide();
            (void)write_slot_verified(1, &record);
            return 0;
        }
        record.revision = 2;
        record.state = VD_PMU_CLEANUP_ARMED;
        record.flags = VD_PMU_CLEANUP_FLAG_OWNER_ARMED;
        record.finished_us = sceKernelGetProcessTimeWide();
        if(write_slot_verified(1, &record) < 0)
        {
            record.results[GATE_RESULT_CLOSE] =
                vdKernelPmuProfilerClose(&record.handles[0]);
            psvDebugScreenPrintf(
                "BLOCKED: armed journal failed; close=%08X\n",
                (uint32_t)record.results[GATE_RESULT_CLOSE]);
            return 0;
        }
        psvDebugScreenPrintf(
            "Owner armed. Process will terminate with live lease.\n");
        if(VD_PMU_CLEANUP_GATE_STAGE ==
           VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT)
        {
            psvDebugScreenPrintf(
                "ARMED: waiting for approved host kill VDCP00013.\n");
            const uint64_t kill_deadline =
                sceKernelGetProcessTimeWide() +
                GATE_KILL_ARM_WINDOW_US;
            while(sceKernelGetProcessTimeWide() < kill_deadline)
                sceKernelDelayThread(100000);
            record.results[GATE_RESULT_CLOSE] =
                vdKernelPmuProfilerClose(&record.handles[0]);
            record.revision = 3;
            record.state = VD_PMU_CLEANUP_FAILED;
            record.flags &= ~VD_PMU_CLEANUP_FLAG_PASS;
            record.finished_us = sceKernelGetProcessTimeWide();
            (void)write_slot_verified(2, &record);
            psvDebugScreenPrintf(
                "FAILED: kill was not observed before active window.\n");
            return 0;
        }
        return 1;
    }

    int action_passed = 0;
    if(VD_PMU_CLEANUP_GATE_STAGE ==
       VD_PMU_CLEANUP_STAGE_CONFLICT)
        action_passed = run_conflict(&record);
    else if(VD_PMU_CLEANUP_GATE_STAGE ==
            VD_PMU_CLEANUP_STAGE_TIMEOUT)
        action_passed = run_timeout(&record);
    else
        action_passed = run_disconnect(&record);
    psvDebugScreenPrintf(
        "Stage %u: %s\n", record.stage,
        finish_stage(&record, 1, action_passed, affinity_before) ?
            "PASS" : "FAIL");
    return 0;
}

static void run_resume(
    const struct vd_pmu_cleanup_record* armed)
{
    struct vd_pmu_cleanup_record record = *armed;
    record.flags = 0;
    const int affinity_before =
        sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    record.results[GATE_RESULT_AFFINITY] = affinity_before < 0 ?
        affinity_before : sceKernelChangeThreadCpuAffinityMask(
            sceKernelGetThreadId(), GATE_CORE_AFFINITY);
    const int passed = finish_stage(
        &record, 2,
        record.results[GATE_RESULT_AFFINITY] >= 0,
        affinity_before);
    psvDebugScreenPrintf(
        "Stage %u resume: %s\n", record.stage,
        passed ? "PASS" : "FAIL");
}

int main(void)
{
    if(psvDebugScreenInit() < 0)
        return 1;
    const int mkdir_result =
        sceIoMkdir("ux0:data/VitaDebugger", 0777);
    struct vd_pmu_cleanup_record latest;
    int latest_slot = -1;
    const int journal_result =
        mkdir_result < 0 && (uint32_t)mkdir_result != GATE_EEXIST ?
            mkdir_result : read_latest(&latest, &latest_slot);
    struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    const int info_result = vdKernelPmuProfilerGetInfo(&info);
    psvDebugScreenPrintf(
        "VitaDebugger PMU cleanup gate stage %u\n",
        VD_PMU_CLEANUP_GATE_STAGE);
    psvDebugScreenPrintf(
        "Opening this UI performs no PMU access.\n");
    psvDebugScreenPrintf(
        "GetInfo=%08X caps=%08X journal=%08X\n",
        (uint32_t)info_result, info.capabilities,
        (uint32_t)journal_result);
    if(journal_result == VD_PMU_CLEANUP_SELECT_EMPTY)
        psvDebugScreenPrintf("X: run this one serialized stage\n");
    else if(journal_result == 0 && latest_slot == 1 &&
            latest.state == VD_PMU_CLEANUP_ARMED)
        psvDebugScreenPrintf(
            "X: verify process cleanup and same-boot re-arm\n");
    else
        psvDebugScreenPrintf(
            "LOCKED: archive all stage journal slots.\n");
    psvDebugScreenPrintf("O: exit without PMU access\n");

    SceCtrlData previous = {0};
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
        if((pressed & SCE_CTRL_CROSS) == 0)
        {
            sceKernelDelayThread(20000);
            continue;
        }
        if(journal_result == VD_PMU_CLEANUP_SELECT_EMPTY)
        {
            if(run_initial(info_result, &info) != 0)
            {
                (void)psvDebugScreenFinish();
                return 0;
            }
        }
        else if(journal_result == 0 && latest_slot == 1 &&
                latest.state == VD_PMU_CLEANUP_ARMED)
            run_resume(&latest);
        else
            psvDebugScreenPrintf("LOCKED: journal state is final.\n");
        psvDebugScreenPrintf("O: exit\n");
    }
    (void)psvDebugScreenFinish();
    return 0;
}
