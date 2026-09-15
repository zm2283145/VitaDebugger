#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/error.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr/misc.h>

#include "pmu_backend.h"
#include "vitadebug_pmu_probe.h"

#define VD_PMU_PROBE_LOCK_RETRY_US 1000u
#define VD_SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)

static volatile int g_probe_lock;
static int g_backend_started;
static int g_module_start_result = VD_PMU_PROBE_ERROR_DISABLED;
static int g_journal_result = VD_PMU_PROBE_JOURNAL_EMPTY;
static int g_latest_valid;
static int g_latest_slot = -1;
static struct vd_pmu_probe_record g_latest;

static void zero_bytes(void* value, size_t size)
{
    volatile uint8_t* bytes = (volatile uint8_t*)value;
    for(size_t i = 0; i < size; ++i)
        bytes[i] = 0;
}

static void copy_bytes(void* destination, const void* source, size_t size)
{
    volatile uint8_t* output = (volatile uint8_t*)destination;
    const volatile uint8_t* input =
        (const volatile uint8_t*)source;
    for(size_t i = 0; i < size; ++i)
        output[i] = input[i];
}

static int bytes_equal(const void* left, const void* right, size_t size)
{
    const volatile uint8_t* lhs = (const volatile uint8_t*)left;
    const volatile uint8_t* rhs = (const volatile uint8_t*)right;
    for(size_t i = 0; i < size; ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static void lock_probe(void)
{
    for(;;)
    {
        int expected = 0;
        if(__atomic_compare_exchange_n(&g_probe_lock, &expected, 1, 0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST))
            return;
        ksceKernelDelayThread(VD_PMU_PROBE_LOCK_RETRY_US);
    }
}

static void unlock_probe(void)
{
    __atomic_store_n(&g_probe_lock, 0, __ATOMIC_SEQ_CST);
}

static const char* record_path(int slot)
{
    return slot == 0 ? VD_PMU_PROBE_RECORD_A_PATH :
                       VD_PMU_PROBE_RECORD_B_PATH;
}

static int read_slot(int slot, struct vd_pmu_probe_record* record)
{
    uint8_t extra;
    SceUID fd = ksceIoOpen(record_path(slot), SCE_O_RDONLY, 0);
    if(fd < 0)
        return fd;

    int result = ksceIoRead(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
    {
        const int extra_result = ksceIoRead(fd, &extra, sizeof(extra));
        result = extra_result == 0 ? 0 :
            (extra_result < 0 ? extra_result : VD_PMU_PROBE_JOURNAL_CORRUPT);
    }
    else if(result >= 0)
    {
        result = VD_PMU_PROBE_JOURNAL_CORRUPT;
    }
    const int close_result = ksceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result < 0)
        return result;
    return vdPmuProbeRecordValid(record) ? 0 :
        VD_PMU_PROBE_JOURNAL_CORRUPT;
}

static int inspect_slot(int slot, struct vd_pmu_probe_record* record,
                        int* present, int* valid)
{
    SceIoStat stat;
    const int stat_result = ksceIoGetstat(record_path(slot), &stat);

    /* Defaults are deliberately fail-closed for an indeterminate stat. */
    *present = 1;
    *valid = 0;
    if(stat_result < 0)
    {
        if((uint32_t)stat_result == VD_SCE_ERROR_ERRNO_ENOENT)
        {
            *present = 0;
            return 0;
        }
        return stat_result;
    }

    const int read_result = read_slot(slot, record);
    if(read_result < 0)
        return read_result;
    *valid = 1;
    return 0;
}

static int read_latest(struct vd_pmu_probe_record* record, int* slot)
{
    struct vd_pmu_probe_record a;
    struct vd_pmu_probe_record b;
    int present_a;
    int present_b;
    int valid_a;
    int valid_b;
    const int result_a = inspect_slot(0, &a, &present_a, &valid_a);
    const int result_b = inspect_slot(1, &b, &present_b, &valid_b);
    const int selected = vdPmuProbeSelectLatest(
        present_a, valid_a, &a, present_b, valid_b, &b);

    if(selected == VD_PMU_PROBE_SELECT_INVALID)
    {
        if((result_a < 0 &&
            result_a != VD_PMU_PROBE_JOURNAL_CORRUPT) ||
           (result_b < 0 &&
            result_b != VD_PMU_PROBE_JOURNAL_CORRUPT))
            return VD_PMU_PROBE_JOURNAL_IO;
        return VD_PMU_PROBE_JOURNAL_CORRUPT;
    }
    if(selected == VD_PMU_PROBE_SELECT_CONFLICT)
        return VD_PMU_PROBE_JOURNAL_CONFLICT;
    if(selected == VD_PMU_PROBE_SELECT_EMPTY)
        return VD_PMU_PROBE_JOURNAL_EMPTY;
    copy_bytes(record, selected == 0 ? &a : &b, sizeof(*record));
    *slot = selected;
    return VD_PMU_PROBE_JOURNAL_OK;
}

static int write_slot_verified(int slot,
                               struct vd_pmu_probe_record* record)
{
    struct vd_pmu_probe_record verification;
    int sync_status = 0;

    record->magic = VD_PMU_PROBE_RECORD_MAGIC;
    record->version = VD_PMU_PROBE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuProbeChecksum(record);

    SceUID fd = ksceIoOpen(record_path(slot),
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           0666);
    if(fd < 0)
        return fd;
    int result = ksceIoWrite(fd, record, sizeof(*record));
    if(result == (int)sizeof(*record))
    {
        result = ksceIoSyncByFd(fd, &sync_status);
        if(result >= 0 && sync_status < 0)
            result = sync_status;
    }
    else if(result >= 0)
    {
        result = VD_PMU_PROBE_JOURNAL_IO;
    }
    const int close_result = ksceIoClose(fd);
    if(result >= 0 && close_result < 0)
        result = close_result;
    if(result >= 0)
        result = ksceIoSync("ux0:", 0);
    if(result < 0)
        return result;
    if(read_slot(slot, &verification) < 0 ||
       !bytes_equal(&verification, record, sizeof(verification)))
        return VD_PMU_PROBE_JOURNAL_CORRUPT;
    return VD_PMU_PROBE_JOURNAL_OK;
}

static int write_next(struct vd_pmu_probe_record* record)
{
    const int target_slot = g_latest_valid && g_latest_slot == 0 ? 1 : 0;
    const int result = write_slot_verified(target_slot, record);
    if(result >= 0)
    {
        copy_bytes(&g_latest, record, sizeof(g_latest));
        g_latest_valid = 1;
        g_latest_slot = target_slot;
        g_journal_result = VD_PMU_PROBE_JOURNAL_OK;
    }
    else
    {
        g_journal_result = result;
    }
    return result;
}

static int refresh_journal(void)
{
    struct vd_pmu_probe_record record;
    int slot = -1;
    const int result = read_latest(&record, &slot);

    g_journal_result = result;
    if(result == VD_PMU_PROBE_JOURNAL_OK)
    {
        copy_bytes(&g_latest, &record, sizeof(g_latest));
        g_latest_valid = 1;
        g_latest_slot = slot;
    }
    else
    {
        zero_bytes(&g_latest, sizeof(g_latest));
        g_latest_valid = 0;
        g_latest_slot = -1;
    }
    return result;
}

static void set_runtime_fields(struct vd_pmu_probe_record* record)
{
    record->module_start_result = g_module_start_result;
    record->backend_ready = vdPmuBackendReady() ? 1u : 0u;
    record->restore_obligation =
        vdPmuBackendHasRestoreObligation() ? 1u : 0u;
    record->flags &= ~(VD_PMU_PROBE_FLAG_BACKEND_READY |
                       VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION |
                       VD_PMU_PROBE_FLAG_JOURNAL_WRITE_FAILED);
    if(record->backend_ready)
        record->flags |= VD_PMU_PROBE_FLAG_BACKEND_READY;
    if(record->restore_obligation)
        record->flags |= VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION;
    record->timestamp_us = (uint64_t)ksceKernelGetSystemTimeWide();
}

static int journal_locked(void)
{
    if(g_journal_result < 0)
        return 1;
    if(!g_latest_valid)
        return 0;
    return !vdPmuProbeRecordPassed(&g_latest);
}

static int requested_core_permitted(uint32_t core)
{
    if(core == 0)
        return 1;
    return g_latest_valid && vdPmuProbeRecordPassed(&g_latest) &&
           (g_latest.passed_core_mask & UINT32_C(1)) != 0;
}

static void initialize_test_result(
    struct vd_pmu_backend_test_result* result, uint32_t core)
{
    zero_bytes(result, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->abi_version = VD_PMU_BACKEND_ABI_VERSION;
    result->core_id = core;
    result->stage = VD_PMU_BACKEND_TEST_NOT_RUN;
    result->operation_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    result->restore_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    result->increment_count = VD_PMU_BACKEND_TEST_INCREMENT_COUNT;
}

static int next_attempt(struct vd_pmu_probe_record* record, uint32_t core)
{
    const uint32_t old_revision = g_latest_valid ? g_latest.revision : 0;
    const uint32_t old_sequence = g_latest_valid ? g_latest.sequence : 0;
    if(old_revision > UINT32_MAX - 3u || old_sequence == UINT32_MAX)
        return VD_PMU_PROBE_ERROR_REVISION;

    zero_bytes(record, sizeof(*record));
    record->revision = old_revision + 1u;
    record->sequence = old_sequence + 1u;
    record->state = VD_PMU_PROBE_STATE_ATTEMPTED;
    record->syscall_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    record->journal_result = VD_PMU_PROBE_JOURNAL_OK;
    record->recovery_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    record->caller_pid = ksceKernelGetProcessId();
    record->requested_core = core;
    record->passed_core_mask =
        g_latest_valid ? g_latest.passed_core_mask : 0;
    initialize_test_result(&record->test, core);
    set_runtime_fields(record);
    return 0;
}

static int persist_transition(struct vd_pmu_probe_record* record,
                              uint32_t state)
{
    if(record->revision == UINT32_MAX)
        return VD_PMU_PROBE_ERROR_REVISION;
    record->revision++;
    record->state = state;
    record->journal_result = VD_PMU_PROBE_JOURNAL_OK;
    set_runtime_fields(record);
    return write_next(record);
}

static int recover_locked(void)
{
    const int pending = vdPmuBackendHasRestoreObligation();
    /* A timed-out command can still be completing on its pinned worker before
     * it publishes a restore flag. The backend's recovery entry point first
     * reaps that late completion, so a disabled backend is also a valid reason
     * to call it even when the obligation bit is not visible yet. */
    if(!pending && vdPmuBackendReady())
        return VD_PMU_PROBE_ERROR_NO_RUNTIME_RESTORE;

    struct vd_pmu_probe_record record;
    int can_journal = g_latest_valid;
    if(can_journal)
    {
        copy_bytes(&record, &g_latest, sizeof(record));
        record.flags |= VD_PMU_PROBE_FLAG_RECOVERY_ATTEMPTED;
        record.recovery_result = VD_PMU_PROBE_RESULT_NOT_RUN;
        if(persist_transition(&record,
                              VD_PMU_PROBE_STATE_RECOVERY_STARTED) < 0)
            can_journal = 0;
    }

    const int result = vdPmuBackendRecover();
    const int obligation = vdPmuBackendHasRestoreObligation();
    if(can_journal)
    {
        record.recovery_result = result;
        record.flags |= VD_PMU_PROBE_FLAG_RECOVERY_ATTEMPTED;
        const int journal_result = persist_transition(
            &record, obligation ? VD_PMU_PROBE_STATE_RESTORE_REQUIRED :
                                  VD_PMU_PROBE_STATE_COMPLETE);
        if(journal_result < 0 && result >= 0)
            return VD_PMU_PROBE_ERROR_JOURNAL;
    }
    if(result < 0 || obligation)
        return result < 0 ? result :
            VD_PMU_PROBE_ERROR_RESTORE_REQUIRED;
    return 0;
}

int vdPmuProbeRunSelfTest(
    uint32_t core_id, struct vd_pmu_backend_test_result* result)
{
    uint32_t syscall_state;
    struct vd_pmu_backend_test_result local_result;
    ENTER_SYSCALL(syscall_state);

    if(!result || core_id >= VD_PMU_BACKEND_APP_CORE_COUNT)
    {
        EXIT_SYSCALL(syscall_state);
        return VD_PMU_PROBE_ERROR_INVALID;
    }
    initialize_test_result(&local_result, core_id);
    int copy_result = ksceKernelMemcpyKernelToUser(
        result, &local_result, sizeof(local_result));
    if(copy_result < 0)
    {
        EXIT_SYSCALL(syscall_state);
        return copy_result;
    }

    lock_probe();
    if(!g_backend_started || !vdPmuBackendReady())
    {
        unlock_probe();
        EXIT_SYSCALL(syscall_state);
        return VD_PMU_PROBE_ERROR_DISABLED;
    }
    const int refresh_result = refresh_journal();
    if(refresh_result < 0 || journal_locked())
    {
        unlock_probe();
        EXIT_SYSCALL(syscall_state);
        return refresh_result < 0 ? VD_PMU_PROBE_ERROR_JOURNAL :
                                    VD_PMU_PROBE_ERROR_LOCKED;
    }
    if(!requested_core_permitted(core_id))
    {
        unlock_probe();
        EXIT_SYSCALL(syscall_state);
        return VD_PMU_PROBE_ERROR_CORE_ORDER;
    }

    struct vd_pmu_probe_record record;
    int operation_result = next_attempt(&record, core_id);
    if(operation_result >= 0)
        operation_result = write_next(&record);
    if(operation_result >= 0)
        operation_result = persist_transition(
            &record, VD_PMU_PROBE_STATE_KERNEL_ENTERED);
    if(operation_result < 0)
    {
        unlock_probe();
        EXIT_SYSCALL(syscall_state);
        return operation_result == VD_PMU_PROBE_ERROR_REVISION ?
            operation_result : VD_PMU_PROBE_ERROR_JOURNAL;
    }

    operation_result = vdPmuBackendRunSelfTest(core_id, &local_result);
    if(local_result.struct_size != sizeof(local_result) ||
       local_result.abi_version != VD_PMU_BACKEND_ABI_VERSION ||
       local_result.core_id != core_id ||
       local_result.increment_count !=
           VD_PMU_BACKEND_TEST_INCREMENT_COUNT)
    {
        initialize_test_result(&local_result, core_id);
        local_result.operation_result = operation_result;
        local_result.restore_result =
            vdPmuBackendHasRestoreObligation() ? operation_result : 0;
    }

    copy_bytes(&record.test, &local_result, sizeof(record.test));
    record.flags |= VD_PMU_PROBE_FLAG_HAS_RESULT;
    record.syscall_result = operation_result;
    set_runtime_fields(&record);
    if(!record.restore_obligation &&
       vdPmuProbeRestorationProven(&record.test))
        record.flags |= VD_PMU_PROBE_FLAG_RESTORE_PROVEN;
    if(!record.restore_obligation && operation_result == 0 &&
       vdPmuProbeTestPassed(&record.test))
        record.passed_core_mask |= UINT32_C(1) << core_id;

    const uint32_t final_state = record.restore_obligation ?
        VD_PMU_PROBE_STATE_RESTORE_REQUIRED :
        VD_PMU_PROBE_STATE_COMPLETE;
    const int final_journal_result =
        persist_transition(&record, final_state);
    if(final_journal_result < 0)
    {
        record.flags |= VD_PMU_PROBE_FLAG_JOURNAL_WRITE_FAILED;
        record.journal_result = final_journal_result;
        record.checksum = 0;
        record.checksum = vdPmuProbeChecksum(&record);
    }

    copy_result = ksceKernelMemcpyKernelToUser(
        result, &local_result, sizeof(local_result));
    unlock_probe();
    EXIT_SYSCALL(syscall_state);
    if(copy_result < 0)
        return copy_result;
    if(record.restore_obligation)
        return operation_result < 0 ? operation_result :
            VD_PMU_PROBE_ERROR_RESTORE_REQUIRED;
    if(final_journal_result < 0)
        return VD_PMU_PROBE_ERROR_JOURNAL;
    return operation_result;
}

int vdPmuProbeGetStatus(struct vd_pmu_probe_status* status)
{
    uint32_t syscall_state;
    struct vd_pmu_probe_status local;
    ENTER_SYSCALL(syscall_state);
    if(!status)
    {
        EXIT_SYSCALL(syscall_state);
        return VD_PMU_PROBE_ERROR_INVALID;
    }

    lock_probe();
    (void)refresh_journal();
    zero_bytes(&local, sizeof(local));
    local.struct_size = sizeof(local);
    local.abi_version = VD_PMU_PROBE_ABI_VERSION;
    local.module_start_result = g_module_start_result;
    local.journal_result = g_journal_result;
    local.backend_ready = vdPmuBackendReady() ? 1u : 0u;
    local.restore_obligation =
        vdPmuBackendHasRestoreObligation() ? 1u : 0u;
    local.locked = journal_locked() || local.restore_obligation;
    if(g_latest_valid)
        copy_bytes(&local.latest, &g_latest, sizeof(local.latest));
    const int result = ksceKernelMemcpyKernelToUser(
        status, &local, sizeof(local));
    unlock_probe();
    EXIT_SYSCALL(syscall_state);
    return result;
}

int vdPmuProbeRecover(void)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    lock_probe();
    (void)refresh_journal();
    const int result = recover_locked();
    unlock_probe();
    EXIT_SYSCALL(syscall_state);
    return result;
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    zero_bytes(&g_latest, sizeof(g_latest));
    g_probe_lock = 0;
    g_backend_started = 0;
    g_latest_valid = 0;
    g_latest_slot = -1;
    (void)ksceIoMkdir("ux0:data/VitaDebugger", 0777);
    (void)refresh_journal();

    /* Worker/event creation only. The backend intentionally performs no PMU
     * register access until the explicit self-test or recovery syscall. */
    g_module_start_result = vdPmuBackendStart();
    g_backend_started = g_module_start_result >= 0;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    lock_probe();
    if(vdPmuBackendHasRestoreObligation())
    {
        const int recovery_result = recover_locked();
        if(recovery_result < 0 || vdPmuBackendHasRestoreObligation())
        {
            unlock_probe();
            return SCE_KERNEL_STOP_FAIL;
        }
    }
    const int result = vdPmuBackendStop();
    if(result < 0)
    {
        unlock_probe();
        return SCE_KERNEL_STOP_FAIL;
    }
    g_backend_started = 0;
    unlock_probe();
    return SCE_KERNEL_STOP_SUCCESS;
}
