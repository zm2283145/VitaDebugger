#include <stdint.h>
#include <stdio.h>

#include "thread_mutation.h"

struct mock_backend {
    SceUID target_guid;
    struct vd_thread_registers core;
    struct vd_thread_vfp_registers vfp;
    int snapshot_core_calls;
    int write_core_calls;
    int snapshot_vfp_calls;
    int write_vfp_calls;
    int fail_snapshot_core_call;
    int fail_write_core_call;
    int fail_snapshot_vfp_call;
    int fail_write_vfp_call;
    int corrupt_core_snapshot_call;
    int retain_calls;
    int release_calls;
    int status_calls;
    int fail_retain_call;
    int target_status;
    int retained;
    char order[32];
    unsigned int order_count;
};

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int core_same(const struct vd_thread_registers* left,
                     const struct vd_thread_registers* right)
{
    for(unsigned int bank = 0; bank < 2u; ++bank)
    {
        for(unsigned int i = 0; i < 13u; ++i)
            if(left->entry[bank].r[i] != right->entry[bank].r[i])
                return 0;
        if(left->entry[bank].sp != right->entry[bank].sp ||
           left->entry[bank].lr != right->entry[bank].lr ||
           left->entry[bank].pc != right->entry[bank].pc ||
           left->entry[bank].cpsr != right->entry[bank].cpsr ||
           left->entry[bank].fpscr != right->entry[bank].fpscr)
            return 0;
    }
    return 1;
}

static int vfp_same(const struct vd_thread_vfp_registers* left,
                    const struct vd_thread_vfp_registers* right)
{
    if(left->layout_version != right->layout_version ||
       left->d_register_count != right->d_register_count)
        return 0;
    for(unsigned int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
        if(left->d[i] != right->d[i])
            return 0;
    return left->fpscr_entry[0] == right->fpscr_entry[0] &&
           left->fpscr_entry[1] == right->fpscr_entry[1];
}

static void note(struct mock_backend* mock, char operation)
{
    if(mock->order_count < sizeof(mock->order))
        mock->order[mock->order_count++] = operation;
}

static int retain_target(
    void* context,
    const struct vd_thread_mutation_identity* target_identity)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->retain_calls++;
    if(!target_identity || target_identity->owner_pid != 10 ||
       target_identity->target_guid != mock->target_guid || mock->retained)
        return -88;
    if(mock->retain_calls == mock->fail_retain_call)
    {
        // Model a backend that retained the process but failed on the thread.
        // The transaction layer must still invoke the idempotent release path.
        mock->retained = 1;
        return -89;
    }
    mock->retained = 1;
    return 0;
}

static void release_target(
    void* context,
    const struct vd_thread_mutation_identity* target_identity)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    if(target_identity && target_identity->owner_pid == 10 &&
       target_identity->target_guid == mock->target_guid && mock->retained)
    {
        mock->release_calls++;
        mock->retained = 0;
    }
}

static int retained_target_status(
    void* context,
    const struct vd_thread_mutation_identity* target_identity)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->status_calls++;
    if(!target_identity || target_identity->owner_pid != 10 ||
       target_identity->target_guid != mock->target_guid || !mock->retained)
        return -1;
    return mock->target_status;
}

static int snapshot_core(void* context, SceUID target_guid,
                         struct vd_thread_registers* registers)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    note(mock, 'c');
    mock->snapshot_core_calls++;
    if(target_guid != mock->target_guid)
        return -90;
    if(mock->snapshot_core_calls == mock->fail_snapshot_core_call)
        return -91;
    *registers = mock->core;
    if(mock->snapshot_core_calls == mock->corrupt_core_snapshot_call)
        registers->entry[0].r[0] ^= 1u;
    return 0;
}

static int write_core(void* context, SceUID target_guid,
                      const struct vd_thread_registers* registers)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    note(mock, 'C');
    mock->write_core_calls++;
    if(target_guid != mock->target_guid)
        return -92;
    if(mock->write_core_calls == mock->fail_write_core_call)
        return -93;
    mock->core = *registers;
    return 0;
}

static int snapshot_vfp(void* context, SceUID target_guid,
                        struct vd_thread_vfp_registers* registers)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    note(mock, 'v');
    mock->snapshot_vfp_calls++;
    if(target_guid != mock->target_guid)
        return -94;
    if(mock->snapshot_vfp_calls == mock->fail_snapshot_vfp_call)
        return -95;
    *registers = mock->vfp;
    return 0;
}

static int write_vfp(void* context, SceUID target_guid,
                     const struct vd_thread_vfp_registers* registers)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    note(mock, 'V');
    mock->write_vfp_calls++;
    if(target_guid != mock->target_guid)
        return -96;
    if(mock->write_vfp_calls == mock->fail_write_vfp_call)
        return -97;
    mock->vfp = *registers;
    return 0;
}

static struct mock_backend make_mock(void)
{
    struct mock_backend mock = {0};
    mock.target_guid = 50;
    mock.target_status = 1;
    for(unsigned int bank = 0; bank < 2u; ++bank)
    {
        for(unsigned int i = 0; i < 13u; ++i)
            mock.core.entry[bank].r[i] = 0x100u * (bank + 1u) + i;
        mock.core.entry[bank].sp = 0x8000u + bank * 0x1000u;
        mock.core.entry[bank].lr = 0x3000u + bank * 0x100u;
        mock.core.entry[bank].pc = 0x4000u + bank * 0x100u;
        mock.core.entry[bank].cpsr = 0x10u | (bank ? 0x20u : 0u);
        mock.core.entry[bank].fpscr = 0x500u + bank;
    }
    mock.vfp.layout_version = VD_KERNEL_VFP_LAYOUT_D32_V1;
    mock.vfp.d_register_count = VD_KERNEL_VFP_D_REGISTER_COUNT;
    for(unsigned int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
        mock.vfp.d[i] = UINT64_C(0x1122334400000000) + i;
    mock.vfp.fpscr_entry[0] = 0x100u;
    mock.vfp.fpscr_entry[1] = 0x200u;
    return mock;
}

static struct vd_thread_mutation_backend make_backend(
    struct mock_backend* mock, unsigned int banks)
{
    const struct vd_thread_mutation_backend backend = {
        .supported_banks = banks,
        .core_writable_register_mask =
            (1u << VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT) - 1u,
        .context = mock,
        .retain_target = retain_target,
        .release_target = release_target,
        .retained_target_status = retained_target_status,
        .snapshot_core = snapshot_core,
        .write_core = write_core,
        .snapshot_vfp = snapshot_vfp,
        .write_vfp = write_vfp,
    };
    return backend;
}

static struct vd_thread_mutation_identity identity(void)
{
    const struct vd_thread_mutation_identity value = {
        .owner_pid = 10,
        .owner_thread = 20,
        .stop_token = 30,
        .target_user_thread = 40,
        .target_guid = 50,
    };
    return value;
}

static struct vd_kernel_thread_mutation_begin_request begin_request(
    unsigned int banks)
{
    const struct vd_kernel_thread_mutation_begin_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_THREAD_MUTATION_ABI_VERSION,
        .stop_token = 30,
        .target_thread = 40,
        .bank_mask = banks,
        .flags = 0,
    };
    return request;
}

static struct vd_kernel_thread_mutation_write_request write_request(
    const struct vd_kernel_thread_mutation_handle* handle,
    unsigned int bank, unsigned int register_bank,
    unsigned int register_index, unsigned int low, unsigned int high)
{
    struct vd_kernel_thread_mutation_write_request request = {0};
    request.handle = *handle;
    request.bank = bank;
    request.register_bank = register_bank;
    request.register_index = register_index;
    request.value_low = low;
    request.value_high = high;
    return request;
}

static void test_capability_and_snapshot_gate(void)
{
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    struct mock_backend mock = make_mock();
    struct vd_thread_mutation_backend backend = make_backend(
        &mock, VD_KERNEL_THREAD_MUTATION_CORE |
               VD_KERNEL_THREAD_MUTATION_VFP);
    check(vdThreadMutationSupportedBanks(&backend) ==
              (VD_KERNEL_THREAD_MUTATION_CORE |
               VD_KERNEL_THREAD_MUTATION_VFP),
          "complete lifetime-safe backends negotiate both banks");
    backend.write_vfp = 0;
    check(vdThreadMutationSupportedBanks(&backend) ==
              VD_KERNEL_THREAD_MUTATION_CORE,
          "missing VFP setter removes only VFP capability");
    backend = make_backend(&mock, VD_KERNEL_THREAD_MUTATION_CORE |
                                  VD_KERNEL_THREAD_MUTATION_VFP);
    backend.core_writable_register_mask = 0;
    check(vdThreadMutationSupportedBanks(&backend) ==
              VD_KERNEL_THREAD_MUTATION_VFP,
          "missing authenticated GPR mask removes only core capability");
    backend.core_writable_register_mask =
        1u << VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT;
    check(vdThreadMutationSupportedBanks(&backend) ==
              VD_KERNEL_THREAD_MUTATION_VFP,
          "unknown GPR-mask bits fail the core capability closed");
    backend = make_backend(&mock, VD_KERNEL_THREAD_MUTATION_CORE |
                                  VD_KERNEL_THREAD_MUTATION_VFP);
    backend.retained_target_status = 0;
    check(vdThreadMutationSupportedBanks(&backend) == 0,
          "missing retained-target lifecycle removes every write capability");

    const struct vd_thread_mutation_identity owner = identity();
    struct vd_kernel_thread_mutation_begin_request request = begin_request(
        VD_KERNEL_THREAD_MUTATION_VFP);
    struct vd_kernel_thread_mutation_handle handle = {0};
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_UNSUPPORTED &&
              mock.snapshot_vfp_calls == 0 &&
              !vdThreadMutationIsActive(&session),
          "unsupported VFP fails before snapshot or mutation");

    backend = make_backend(&mock, VD_KERNEL_THREAD_MUTATION_CORE);
    request = begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_begin_request invalid = request;
    invalid.abi_version++;
    check(vdThreadMutationBegin(&session, &owner, &invalid, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_INVALID &&
              mock.snapshot_core_calls == 0,
          "begin rejects an unknown mutation ABI before snapshot");
    invalid = request;
    invalid.struct_size--;
    check(vdThreadMutationBegin(&session, &owner, &invalid, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_INVALID &&
              mock.snapshot_core_calls == 0,
          "begin rejects a mismatched request size before snapshot");
    invalid = request;
    invalid.bank_mask = 4u;
    check(vdThreadMutationBegin(&session, &owner, &invalid, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_INVALID &&
              mock.snapshot_core_calls == 0,
          "begin rejects unknown bank bits before snapshot");
    invalid = request;
    invalid.bank_mask = VD_KERNEL_THREAD_MUTATION_CORE |
                        VD_KERNEL_THREAD_MUTATION_VFP;
    check(vdThreadMutationBegin(&session, &owner, &invalid, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_INVALID &&
              mock.retain_calls == 0 && mock.snapshot_core_calls == 0,
          "one transaction cannot mix aliased core and VFP banks");
    invalid = request;
    invalid.flags = 1u;
    check(vdThreadMutationBegin(&session, &owner, &invalid, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_INVALID &&
              mock.snapshot_core_calls == 0,
          "begin rejects unnegotiated flags before snapshot");

    backend.core_writable_register_mask = 1u << 4u;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) == 0,
          "begin accepts an authenticated subset writable mask");
    struct vd_kernel_thread_mutation_write_request masked_write =
        write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 5u,
                      0x12345678u, 0u);
    check(vdThreadMutationStage(&session, &owner, &masked_write,
                                &backend) ==
              VD_KERNEL_ERROR_MUTATION_UNSUPPORTED &&
              mock.write_core_calls == 0,
          "stage rejects a GPR absent from the authenticated contract");
    check(vdThreadMutationRestore(&session, &owner, &handle,
                                  &backend) == 0,
          "masked-register rejection leaves an exactly restorable lease");
    backend = make_backend(&mock, VD_KERNEL_THREAD_MUTATION_CORE);

    mock.core.entry[0].cpsr = 0x13u;
    mock.core.entry[1].cpsr = 0x13u;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_TARGET &&
              !vdThreadMutationIsActive(&session),
          "begin rejects a core snapshot with no resumable user bank");
    mock = make_mock();
    mock.fail_retain_call = 1;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) == -89 &&
              mock.retain_calls == 1 && mock.release_calls == 1 &&
              !mock.retained && !vdThreadMutationIsActive(&session),
          "partial retained-target acquisition is always released");
    mock = make_mock();
    mock.fail_snapshot_core_call = 1;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) == -91 &&
              mock.write_core_calls == 0 &&
              !vdThreadMutationIsActive(&session),
          "snapshot failure cannot create a write transaction");
    mock.fail_snapshot_core_call = 0;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) == 0,
          "begin a single bounded transaction");
    const int snapshots_before_busy = mock.snapshot_core_calls;
    check(vdThreadMutationBegin(&session, &owner, &request, &backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_BUSY &&
              mock.snapshot_core_calls == snapshots_before_busy,
          "second transaction fails busy without touching the target");
    check(vdThreadMutationRestore(&session, &owner, &handle, &backend) == 0,
          "cancel capability fixture transaction");
    check(mock.retain_calls == mock.release_calls && !mock.retained,
          "snapshot-only cancellation releases the exact target reference");
}

static void test_exact_owner_bounds_commit_restore(void)
{
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_thread_registers original_core = mock.core;
    const struct vd_thread_mutation_backend backend = make_backend(
        &mock, VD_KERNEL_THREAD_MUTATION_CORE |
               VD_KERNEL_THREAD_MUTATION_VFP);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0 &&
              mock.order_count == 1u && mock.order[0] == 'c' &&
              mock.snapshot_vfp_calls == 0 && mock.write_core_calls == 0 &&
              mock.write_vfp_calls == 0,
          "begin snapshots the one requested bank before any write");

    struct vd_thread_mutation_identity wrong = owner;
    wrong.owner_pid++;
    check(vdThreadMutationCommit(&session, &wrong, &handle, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "commit rejects wrong process owner");
    wrong = owner;
    wrong.owner_thread++;
    check(vdThreadMutationCommit(&session, &wrong, &handle, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "commit rejects wrong controller thread");
    wrong = owner;
    wrong.target_guid++;
    check(vdThreadMutationCommit(&session, &wrong, &handle, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "commit rejects a different exact target GUID");
    struct vd_kernel_thread_mutation_handle stale = handle;
    stale.generation++;
    check(vdThreadMutationCommit(&session, &owner, &stale, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "commit rejects stale generation");

    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 2u, 0u, 1u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "core bank bounds fail closed");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT,
                          1u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "core register bounds fail closed");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0u, 1u, 1u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "32-bit core write rejects high payload");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          16u, 0x13u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "CPSR privilege-mode mutation fails closed");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 1u,
                          0u, 1u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_TARGET,
          "inactive raw core bank fails closed when entry zero is user state");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 0u,
                          0u, 1u, 2u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
               VD_KERNEL_ERROR_MUTATION_INVALID,
          "handle cannot stage the unselected VFP bank");

    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0u, 0xaabbccddu, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0 &&
              mock.core.entry[0].r[0] == 0xaabbccddu &&
              vdThreadMutationIsActive(&session),
          "stage applies and verifies a provisional core register");
    check(vdThreadMutationCleanup(&session, owner.owner_pid + 1,
                                  owner.stop_token, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "cleanup rejects a different process/stop owner");
    check(vdThreadMutationRestore(&session, &owner, &stale, &backend) ==
              VD_KERNEL_ERROR_MUTATION_OWNER,
          "restore rejects a stale generation");
    check(vdThreadMutationRestore(&session, &owner, &handle, &backend) == 0 &&
              core_same(&mock.core, &original_core) &&
              !vdThreadMutationIsActive(&session),
          "explicit restore verifies the exact original core snapshot");

    struct vd_kernel_thread_mutation_handle committed;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &committed) == 0,
          "begin explicit commit fixture");
    write = write_request(&committed, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0u, 0xaabbccddu, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0,
          "stage committed core value");
    check(vdThreadMutationCommit(&session, &owner, &committed,
                                 &backend) == 0 &&
              mock.core.entry[0].r[0] == 0xaabbccddu &&
              !vdThreadMutationIsActive(&session),
          "commit accepts verified target state and retires restore metadata");
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_STATE,
          "committed handle is stale after transaction retirement");
}

static void test_vfp_bounds_commit_restore(void)
{
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_thread_vfp_registers original = mock.vfp;
    const struct vd_thread_mutation_backend backend = make_backend(
        &mock, VD_KERNEL_THREAD_MUTATION_CORE |
               VD_KERNEL_THREAD_MUTATION_VFP);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_VFP);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0 &&
              mock.snapshot_core_calls == 0 && mock.snapshot_vfp_calls == 1,
          "VFP transaction snapshots only the VFP bank");

    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_VFP, 1u, 0u, 1u, 2u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "VFP bank bounds fail closed");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 0u,
                          VD_KERNEL_THREAD_MUTATION_VFP_REGISTER_COUNT,
                          1u, 2u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "VFP register bounds fail closed");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 0u,
                          VD_KERNEL_THREAD_MUTATION_VFP_FPSCR_INDEX,
                          1u, 1u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_INVALID,
          "32-bit FPSCR write rejects a high payload");

    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 0u,
                          31u, 0x55667788u, 0x11223344u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0 &&
              mock.vfp.d[31] == UINT64_C(0x1122334455667788),
          "stage applies and verifies a provisional VFP D register");
    check(vdThreadMutationRestore(&session, &owner, &handle, &backend) == 0 &&
              vfp_same(&mock.vfp, &original) && !mock.retained,
          "VFP restore verifies the original snapshot and releases target");

    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0,
          "begin VFP commit fixture");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 0u,
                          VD_KERNEL_THREAD_MUTATION_VFP_FPSCR_INDEX,
                          0x12340000u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0 &&
              vdThreadMutationCommit(&session, &owner, &handle,
                                     &backend) == 0 &&
              mock.vfp.fpscr_entry[0] == 0x12340000u && !mock.retained,
          "single-bank FPSCR commit avoids core/VFP alias ambiguity");
}

static void test_rollback_and_retryable_cleanup(void)
{
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_thread_registers original = mock.core;
    const struct vd_thread_mutation_backend backend = make_backend(
        &mock, VD_KERNEL_THREAD_MUTATION_CORE);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0,
          "begin rollback fixture");
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0u, 0xfeedfaceu, 0u);
    mock.corrupt_core_snapshot_call = 2;
    check(vdThreadMutationStage(&session, &owner, &write, &backend) ==
              VD_KERNEL_ERROR_MUTATION_VERIFY &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session),
          "stage verification failure rolls back exactly before returning");

    mock = make_mock();
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0,
          "begin commit verification fixture");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0u, 0xfeedfaceu, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0,
          "stage commit verification fixture");
    mock.corrupt_core_snapshot_call = 3;
    check(vdThreadMutationCommit(&session, &owner, &handle, &backend) ==
              VD_KERNEL_ERROR_MUTATION_VERIFY &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session),
          "commit re-verification failure restores before returning");

    mock = make_mock();
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &handle) == 0,
          "begin restore retry fixture");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0u, 0xfeedfaceu, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0,
          "stage restore retry fixture");
    mock.fail_write_core_call = 2;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, &backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.core.entry[0].r[0] == 0xfeedfaceu,
          "failed lease cleanup retains the restore obligation");
    mock.fail_write_core_call = 0;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, &backend) == 0 &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session),
          "later lease cleanup retries and verifies exact restoration");
}

static void test_generation_and_target_lifecycle(void)
{
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_thread_mutation_backend backend = make_backend(
        &mock, VD_KERNEL_THREAD_MUTATION_CORE);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle first;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &first) == 0 &&
          vdThreadMutationRestore(&session, &owner, &first, &backend) == 0,
          "cancel snapshot-only transaction without a target write");
    session.next_sequence = UINT64_C(0x100000001);
    struct vd_kernel_thread_mutation_handle second;
    check(vdThreadMutationBegin(&session, &owner, &begin, &backend,
                                &second) == 0 &&
              second.token == first.token &&
              second.generation != first.generation,
          "generation distinguishes an intentionally reused token");
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &second, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0u, 0x12345678u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, &backend) == 0,
          "stage target-lifecycle fixture");
    const int writes_before = mock.write_core_calls;
    mock.target_status = -1;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, &backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before,
          "unknown retained-target state blocks unsafe cleanup write");
    mock.target_status = 0;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, &backend) == 0 &&
              !vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before,
          "retained-target destruction proof clears obsolete restore metadata");
}

int main(void)
{
    check(sizeof(struct vd_kernel_thread_mutation_info) == 32u,
          "mutation info ABI size");
    check(sizeof(struct vd_kernel_thread_mutation_begin_request) == 24u,
          "begin request ABI size");
    check(sizeof(struct vd_kernel_thread_mutation_handle) == 32u,
          "mutation handle ABI size");
    check(sizeof(struct vd_kernel_thread_mutation_write_request) == 56u,
          "write request ABI size");
    test_capability_and_snapshot_gate();
    test_exact_owner_bounds_commit_restore();
    test_vfp_bounds_commit_restore();
    test_rollback_and_retryable_cleanup();
    test_generation_and_target_lifecycle();
    if(failures)
        return 1;
    puts("PASS: bounded kernel thread-mutation transaction lifecycle");
    return 0;
}
