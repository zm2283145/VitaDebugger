#include <stdint.h>
#include <stdio.h>

#include "thread_mutation.h"
#include "thread_mutation_provider.h"

#define MOCK_PROCESS_OBJECT ((uintptr_t)0x10001000u)
#define MOCK_THREAD_OBJECT ((uintptr_t)0x20002000u)
#define MOCK_REUSED_THREAD_OBJECT ((uintptr_t)0x30003000u)
#define MOCK_PROCESS_GENERATION UINT64_C(0x100000001)
#define MOCK_THREAD_GENERATION UINT64_C(0x200000002)

struct mock_platform {
    struct vd_thread_setter_binding binding;
    struct vd_thread_target_reference process_reference;
    struct vd_thread_target_reference thread_reference;
    struct vd_thread_registers core;
    struct vd_thread_vfp_registers vfp;
    int resolve_result;
    int binding_state;
    int target_state;
    int process_alive;
    int thread_alive;
    int relationship_valid;
    int thread_class_valid;
    int process_held;
    int thread_held;
    int access_active;
    int resolve_calls;
    int binding_status_calls;
    int retain_process_calls;
    int retain_thread_calls;
    int release_calls;
    int status_calls;
    int snapshot_core_calls;
    int write_core_calls;
    int snapshot_vfp_calls;
    int write_vfp_calls;
    int begin_access_calls;
    int end_access_calls;
    int fail_begin_access_call;
    int fail_retain_process;
    int fail_retain_thread;
    int leak_process_on_failed_retain;
    int leak_thread_on_failed_retain;
    int retry_release_call;
    int ambiguous_release_call;
    int fail_write_core_call;
    int corrupt_core_snapshot_call;
    uintptr_t uid_thread_object;
    uintptr_t last_core_object;
    uintptr_t last_vfp_object;
    int observed_uid_reuse;
    unsigned int binding_errors;
    char release_order[8];
    unsigned int release_order_count;
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
    for(unsigned int bank = 0;
        bank < VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT; ++bank)
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

static int binding_matches(
    struct mock_platform* mock,
    const struct vd_thread_setter_binding* binding)
{
    int valid = binding && binding->module_id == mock->binding.module_id &&
                binding->struct_size == mock->binding.struct_size &&
                binding->version == mock->binding.version &&
                binding->supported_banks == mock->binding.supported_banks &&
                binding->module_nid == mock->binding.module_nid &&
                binding->core_get == mock->binding.core_get &&
                binding->core_set == mock->binding.core_set &&
                binding->vfp_get == mock->binding.vfp_get &&
                binding->vfp_set == mock->binding.vfp_set;
    if(valid)
    {
        for(unsigned int i = 0; i < VD_THREAD_SETTER_FINGERPRINT_SIZE; ++i)
        {
            if(binding->fingerprint[i] != mock->binding.fingerprint[i])
            {
                valid = 0;
                break;
            }
        }
    }
    if(!valid)
        mock->binding_errors++;
    return valid;
}

static int resolve_verified_binding(
    void* context, struct vd_thread_setter_binding* binding)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->resolve_calls++;
    if(mock->resolve_result < 0)
        return mock->resolve_result;
    *binding = mock->binding;
    return 0;
}

static int binding_status(
    void* context, const struct vd_thread_setter_binding* binding)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->binding_status_calls++;
    if(!binding_matches(mock, binding))
        return -301;
    return mock->binding_state;
}

static int retain_process(
    void* context, SceUID owner_pid,
    struct vd_thread_target_reference* reference)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->retain_process_calls++;
    if(mock->fail_retain_process)
    {
        if(mock->leak_process_on_failed_retain)
        {
            mock->process_held = 1;
            *reference = mock->process_reference;
        }
        return mock->fail_retain_process;
    }
    if(owner_pid != mock->process_reference.guid || mock->process_held)
        return -302;
    mock->process_held = 1;
    *reference = mock->process_reference;
    return 0;
}

static int retain_thread(
    void* context, SceUID target_guid,
    struct vd_thread_target_reference* reference)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->retain_thread_calls++;
    if(mock->fail_retain_thread)
    {
        if(mock->leak_thread_on_failed_retain)
        {
            mock->thread_held = 1;
            *reference = mock->thread_reference;
        }
        return mock->fail_retain_thread;
    }
    if(target_guid != mock->thread_reference.guid || mock->thread_held)
        return -303;
    mock->thread_held = 1;
    *reference = mock->thread_reference;
    return 0;
}

static int reference_same(
    const struct vd_thread_target_reference* left,
    const struct vd_thread_target_reference* right)
{
    return left && right && left->guid == right->guid &&
           left->object == right->object &&
           left->generation == right->generation;
}

static int release_reference(
    void* context, const struct vd_thread_target_reference* reference)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->release_calls++;
    if(mock->release_calls == mock->retry_release_call)
        return VD_THREAD_REFERENCE_RELEASE_RETAINED;
    if(reference_same(reference, &mock->thread_reference) &&
       mock->thread_held)
    {
        mock->thread_held = 0;
        if(mock->release_order_count < sizeof(mock->release_order))
            mock->release_order[mock->release_order_count++] = 't';
        return mock->release_calls == mock->ambiguous_release_call ?
                   -304 : VD_THREAD_REFERENCE_RELEASED;
    }
    if(reference_same(reference, &mock->process_reference) &&
       mock->process_held)
    {
        mock->process_held = 0;
        if(mock->release_order_count < sizeof(mock->release_order))
            mock->release_order[mock->release_order_count++] = 'p';
        return mock->release_calls == mock->ambiguous_release_call ?
                   -304 : VD_THREAD_REFERENCE_RELEASED;
    }
    return -305;
}

static int retained_target_status(
    void* context,
    const struct vd_thread_target_reference* process,
    const struct vd_thread_target_reference* thread,
    const struct vd_thread_mutation_identity* identity)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->status_calls++;
    if(!identity || identity->owner_pid != mock->process_reference.guid ||
       identity->owner_thread != 20 || identity->stop_token != 30u ||
       identity->target_user_thread != 40 ||
       identity->target_guid != mock->thread_reference.guid ||
       !reference_same(process, &mock->process_reference) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->process_held || !mock->thread_held ||
       !mock->relationship_valid || !mock->thread_class_valid)
        return -306;
    if(!mock->process_alive || !mock->thread_alive)
        return 0;
    return mock->target_state;
}

static int begin_target_access(
    void* context, const struct vd_thread_setter_binding* binding,
    const struct vd_thread_target_reference* process,
    const struct vd_thread_target_reference* thread,
    const struct vd_thread_mutation_identity* identity)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->begin_access_calls++;
    if(mock->begin_access_calls == mock->fail_begin_access_call)
        return -312;
    if(mock->access_active || !binding_matches(mock, binding) || !identity ||
       identity->owner_pid != mock->process_reference.guid ||
       identity->owner_thread != 20 || identity->stop_token != 30u ||
       identity->target_user_thread != 40 ||
       identity->target_guid != mock->thread_reference.guid ||
       !reference_same(process, &mock->process_reference) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->process_held || !mock->thread_held ||
       !mock->process_alive || !mock->thread_alive ||
       !mock->relationship_valid || !mock->thread_class_valid ||
       mock->binding_state != 1 || mock->target_state != 1)
        return -312;
    mock->access_active = 1;
    return 1;
}

static void end_target_access(void* context)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    if(mock->access_active)
    {
        mock->access_active = 0;
        mock->end_access_calls++;
    }
}

static int snapshot_core(
    void* context, const struct vd_thread_setter_binding* binding,
    const struct vd_thread_target_reference* thread,
    struct vd_thread_registers* registers)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->snapshot_core_calls++;
    if(!binding_matches(mock, binding) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->thread_held || !mock->access_active)
        return -307;
    mock->last_core_object = thread->object;
    if(thread->object != mock->uid_thread_object)
        mock->observed_uid_reuse = 1;
    *registers = mock->core;
    if(mock->snapshot_core_calls == mock->corrupt_core_snapshot_call)
        registers->entry[0].r[0] ^= 1u;
    return 0;
}

static int write_core(
    void* context, const struct vd_thread_setter_binding* binding,
    const struct vd_thread_target_reference* thread,
    const struct vd_thread_registers* registers)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->write_core_calls++;
    if(!binding_matches(mock, binding) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->thread_held || !mock->access_active)
        return -308;
    if(mock->write_core_calls == mock->fail_write_core_call)
        return -309;
    mock->last_core_object = thread->object;
    if(thread->object != mock->uid_thread_object)
        mock->observed_uid_reuse = 1;
    mock->core = *registers;
    return 0;
}

static int snapshot_vfp(
    void* context, const struct vd_thread_setter_binding* binding,
    const struct vd_thread_target_reference* thread,
    struct vd_thread_vfp_registers* registers)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->snapshot_vfp_calls++;
    if(!binding_matches(mock, binding) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->thread_held || !mock->access_active)
        return -310;
    mock->last_vfp_object = thread->object;
    *registers = mock->vfp;
    return 0;
}

static int write_vfp(
    void* context, const struct vd_thread_setter_binding* binding,
    const struct vd_thread_target_reference* thread,
    const struct vd_thread_vfp_registers* registers)
{
    struct mock_platform* mock = (struct mock_platform*)context;
    mock->write_vfp_calls++;
    if(!binding_matches(mock, binding) ||
       !reference_same(thread, &mock->thread_reference) ||
       !mock->thread_held || !mock->access_active)
        return -311;
    mock->last_vfp_object = thread->object;
    mock->vfp = *registers;
    return 0;
}

static struct mock_platform make_mock(void)
{
    struct mock_platform mock = {0};
    mock.binding.struct_size = sizeof(mock.binding);
    mock.binding.version = VD_THREAD_SETTER_BINDING_VERSION;
    mock.binding.supported_banks = VD_KERNEL_THREAD_MUTATION_CORE |
                                   VD_KERNEL_THREAD_MUTATION_VFP;
    mock.binding.module_id = 100;
    mock.binding.module_nid = 0x11223344u;
    mock.binding.core_get = (uintptr_t)0x81001000u;
    mock.binding.core_set = (uintptr_t)0x81002000u;
    mock.binding.vfp_get = (uintptr_t)0x81003000u;
    mock.binding.vfp_set = (uintptr_t)0x81004000u;
    for(unsigned int i = 0; i < VD_THREAD_SETTER_FINGERPRINT_SIZE; ++i)
        mock.binding.fingerprint[i] = (unsigned char)(i + 1u);
    mock.process_reference.guid = 10;
    mock.process_reference.object = MOCK_PROCESS_OBJECT;
    mock.process_reference.generation = MOCK_PROCESS_GENERATION;
    mock.thread_reference.guid = 50;
    mock.thread_reference.object = MOCK_THREAD_OBJECT;
    mock.thread_reference.generation = MOCK_THREAD_GENERATION;
    mock.resolve_result = 0;
    mock.binding_state = 1;
    mock.target_state = 1;
    mock.process_alive = 1;
    mock.thread_alive = 1;
    mock.relationship_valid = 1;
    mock.thread_class_valid = 1;
    mock.uid_thread_object = MOCK_THREAD_OBJECT;
    for(unsigned int bank = 0;
        bank < VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT; ++bank)
    {
        for(unsigned int i = 0; i < 13u; ++i)
            mock.core.entry[bank].r[i] = 0x100u * (bank + 1u) + i;
        mock.core.entry[bank].sp = 0x8000u + bank * 0x1000u;
        mock.core.entry[bank].lr = 0x3000u + bank * 0x100u;
        mock.core.entry[bank].pc = 0x4000u + bank * 0x100u;
        mock.core.entry[bank].cpsr = bank == 0 ? 0x10u : 0x13u;
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

static struct vd_thread_mutation_provider_ops make_ops(
    struct mock_platform* mock)
{
    const struct vd_thread_mutation_provider_ops ops = {
        .context = mock,
        .resolve_verified_binding = resolve_verified_binding,
        .binding_status = binding_status,
        .retain_process = retain_process,
        .retain_thread = retain_thread,
        .release_reference = release_reference,
        .begin_target_access = begin_target_access,
        .end_target_access = end_target_access,
        .retained_target_status = retained_target_status,
        .snapshot_core = snapshot_core,
        .write_core = write_core,
        .snapshot_vfp = snapshot_vfp,
        .write_vfp = write_vfp,
    };
    return ops;
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
    unsigned int bank)
{
    const struct vd_kernel_thread_mutation_begin_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_THREAD_MUTATION_ABI_VERSION,
        .stop_token = 30,
        .target_thread = 40,
        .bank_mask = bank,
        .flags = 0,
    };
    return request;
}

static struct vd_kernel_thread_mutation_write_request write_request(
    const struct vd_kernel_thread_mutation_handle* handle,
    unsigned int bank, unsigned int register_index,
    unsigned int value_low, unsigned int value_high)
{
    struct vd_kernel_thread_mutation_write_request request = {0};
    request.handle = *handle;
    request.bank = bank;
    request.register_index = register_index;
    request.value_low = value_low;
    request.value_high = value_high;
    return request;
}

static void test_binding_gate(void)
{
    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    check(vdThreadMutationProviderInit(&provider, &ops) == 0,
          "initialize verified provider once");
    check(vdThreadMutationProviderReady(&provider),
          "verified binding enables the provider");
    check(vdThreadMutationSupportedBanks(
              vdThreadMutationProviderBackend(&provider)) ==
              (VD_KERNEL_THREAD_MUTATION_CORE |
               VD_KERNEL_THREAD_MUTATION_VFP),
          "verified binding advertises only authenticated banks");

    mock = make_mock();
    ops = make_ops(&mock);
    mock.binding.fingerprint[0] = 0;
    for(unsigned int i = 1; i < VD_THREAD_SETTER_FINGERPRINT_SIZE; ++i)
        mock.binding.fingerprint[i] = 0;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider) &&
              vdThreadMutationSupportedBanks(
                  vdThreadMutationProviderBackend(&provider)) == 0,
          "zero code fingerprint fails closed");

    mock = make_mock();
    ops = make_ops(&mock);
    mock.binding.core_set = 0;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider),
          "missing setter address fails closed");

    mock = make_mock();
    ops = make_ops(&mock);
    mock.binding.supported_banks |= 1u << 8;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider),
          "unknown writable bank fails closed");

    mock = make_mock();
    ops = make_ops(&mock);
    mock.resolve_result = -401;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider),
          "failed symbol authentication fails closed");

    mock = make_mock();
    ops = make_ops(&mock);
    mock.binding_state = -1;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider),
          "uncertain binding liveness fails closed");
    mock.binding_state = 1;
    check(vdThreadMutationProviderReady(&provider) &&
              vdThreadMutationSupportedBanks(
                  vdThreadMutationProviderBackend(&provider)) ==
                  (VD_KERNEL_THREAD_MUTATION_CORE |
                   VD_KERNEL_THREAD_MUTATION_VFP),
          "later authenticated liveness check can re-enable a clean provider");

    mock = make_mock();
    ops = make_ops(&mock);
    ops.begin_target_access = 0;
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    check(!vdThreadMutationProviderReady(&provider),
          "missing atomic access gate keeps every writable bank disabled");
}

static void test_exact_reference_core_and_vfp(void)
{
    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    struct vd_kernel_thread_mutation_handle handle;
    struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    const struct vd_thread_registers original_core = mock.core;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0 &&
              provider.process.object == MOCK_PROCESS_OBJECT &&
              provider.thread.object == MOCK_THREAD_OBJECT,
          "begin retains the exact process and thread objects");

    mock.uid_thread_object = MOCK_REUSED_THREAD_OBJECT;
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0xaabbccddu, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0 &&
              mock.core.entry[0].r[0] == 0xaabbccddu &&
              mock.last_core_object == MOCK_THREAD_OBJECT &&
              mock.observed_uid_reuse,
          "provider preserves the retained object after the GUID mapping changes");
    check(vdThreadMutationRestore(&session, &owner, &handle, backend) == 0 &&
              core_same(&mock.core, &original_core) &&
              mock.release_order_count == 2u &&
              mock.release_order[0] == 't' &&
              mock.release_order[1] == 'p' &&
              vdThreadMutationProviderReady(&provider),
          "core restore is exact and releases thread before process");

    const struct vd_thread_vfp_registers original_vfp = mock.vfp;
    begin = begin_request(VD_KERNEL_THREAD_MUTATION_VFP);
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin retains a fresh target for VFP");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_VFP, 31u,
                          0x55667788u, 0x11223344u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0 &&
              mock.vfp.d[31] == UINT64_C(0x1122334455667788) &&
              mock.last_vfp_object == MOCK_THREAD_OBJECT,
          "VFP write uses the exact retained thread object");
    check(vdThreadMutationRestore(&session, &owner, &handle, backend) == 0 &&
              vfp_same(&mock.vfp, &original_vfp) &&
              mock.binding_errors == 0 && !mock.access_active &&
              mock.begin_access_calls == mock.end_access_calls,
          "VFP restore returns the complete bank and releases every access gate");
}

static void test_partial_retain_unwind(void)
{
    struct mock_platform mock = make_mock();
    mock.fail_retain_thread = -410;
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(
              &session, &owner, &begin,
              vdThreadMutationProviderBackend(&provider), &handle) == -410,
          "thread retain failure is returned unchanged");
    check(!vdThreadMutationIsActive(&session) &&
              !mock.process_held && !mock.thread_held &&
              mock.release_calls == 1 &&
              mock.release_order_count == 1u &&
              mock.release_order[0] == 'p' &&
              vdThreadMutationProviderReady(&provider),
          "partial retain failure releases the process reference");

    mock = make_mock();
    mock.relationship_valid = 0;
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(
              &session, &owner, &begin,
              vdThreadMutationProviderBackend(&provider), &handle) ==
              VD_KERNEL_ERROR_MUTATION_TARGET &&
              !vdThreadMutationIsActive(&session) &&
              !mock.process_held && !mock.thread_held,
          "invalid retained process/thread relation unwinds both references");
}

static void test_failed_retain_contract_quarantine(void)
{
    struct mock_platform mock = make_mock();
    mock.fail_retain_process = -411;
    mock.leak_process_on_failed_retain = 1;
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(
              &session, &owner, &begin,
              vdThreadMutationProviderBackend(&provider), &handle) == -411,
          "process retain failure is returned unchanged");
    const int releases_before = mock.release_calls;
    check(!vdThreadMutationIsActive(&session) &&
              provider.acquisition_uncertain &&
              provider.release_uncertain && provider.release_pending &&
              mock.process_held &&
              vdThreadMutationSupportedBanks(
                  vdThreadMutationProviderBackend(&provider)) == 0 &&
              vdThreadMutationProviderDrain(&provider) == -411 &&
              vdThreadMutationProviderPrepareUnload(&provider) == -411 &&
              mock.release_calls == releases_before,
          "failed process retain with output is quarantined without release");

    mock = make_mock();
    mock.fail_retain_thread = -412;
    mock.leak_thread_on_failed_retain = 1;
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(
              &session, &owner, &begin,
              vdThreadMutationProviderBackend(&provider), &handle) == -412,
          "thread retain failure is returned unchanged");
    check(provider.acquisition_uncertain &&
              mock.process_held && mock.thread_held &&
              mock.release_calls == 0 &&
              vdThreadMutationProviderDrain(&provider) == -412,
          "failed thread retain with output quarantines both references");

    mock = make_mock();
    mock.thread_reference.object = MOCK_PROCESS_OBJECT;
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(
              &session, &owner, &begin,
              vdThreadMutationProviderBackend(&provider), &handle) ==
              VD_KERNEL_ERROR_MUTATION_TARGET &&
              provider.acquisition_uncertain &&
              mock.process_held && mock.thread_held &&
              mock.release_calls == 0,
          "successful retain with aliased objects is fatally quarantined");
}

static void test_rollback_and_transient_liveness(void)
{
    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    const struct vd_thread_registers original = mock.core;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin read-back rollback fixture");
    mock.corrupt_core_snapshot_call = 2;
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0x12345678u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) ==
              VD_KERNEL_ERROR_MUTATION_VERIFY &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session) &&
              !mock.process_held && !mock.thread_held,
          "failed read-back rolls back exactly and releases references");

    mock = make_mock();
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    backend = vdThreadMutationProviderBackend(&provider);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin transient binding fixture");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0x87654321u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0,
          "stage before transient binding loss");
    mock.binding_state = -1;
    const int writes_before = mock.write_core_calls;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before &&
              vdThreadMutationSupportedBanks(backend) == 0,
          "unknown binding state preserves restore obligation and blocks writes");
    mock.binding_state = 1;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) == 0 &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session) &&
              vdThreadMutationProviderReady(&provider),
          "verified binding recovery permits exact cleanup retry");

    mock = make_mock();
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    backend = vdThreadMutationProviderBackend(&provider);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin atomic-access failure fixture");
    mock.fail_begin_access_call = mock.begin_access_calls + 1;
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0xfedcba98u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) ==
              VD_KERNEL_ERROR_MUTATION_TARGET &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session) &&
              !mock.access_active && !mock.process_held && !mock.thread_held,
          "access-gate race failure rolls back without invoking an unsafe setter");
}

static void run_destroyed_target_case(int destroy_process)
{
    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          destroy_process ? "begin process-exit fixture" :
                            "begin thread-exit fixture");
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0x44556677u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0,
          destroy_process ? "stage process-exit fixture" :
                            "stage thread-exit fixture");
    const int writes_before = mock.write_core_calls;
    if(destroy_process)
        mock.process_alive = 0;
    else
        mock.thread_alive = 0;
    mock.uid_thread_object = MOCK_REUSED_THREAD_OBJECT;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) == 0 &&
              !vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before &&
              !mock.process_held && !mock.thread_held,
          destroy_process ?
              "proven process exit retires obsolete restore without writing" :
              "proven thread exit retires obsolete restore without writing");
}

static void test_target_exit_and_unknown_state(void)
{
    run_destroyed_target_case(0);
    run_destroyed_target_case(1);

    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    const struct vd_thread_registers original = mock.core;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin unknown-target fixture");
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u, 0xabcdef01u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0,
          "stage unknown-target fixture");
    mock.target_state = -1;
    const int writes_before = mock.write_core_calls;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before &&
              mock.process_held && mock.thread_held &&
              vdThreadMutationSupportedBanks(backend) == 0,
          "unknown target state cannot discard or redirect restore");
    mock.target_state = 1;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) == 0 &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session),
          "later exact-target proof completes deferred restoration");

    mock = make_mock();
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    backend = vdThreadMutationProviderBackend(&provider);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin parent-change fixture");
    write = write_request(&handle, VD_KERNEL_THREAD_MUTATION_CORE, 0u,
                          0x10203040u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0,
          "stage before retained parent change");
    mock.relationship_valid = 0;
    const int parent_change_writes = mock.write_core_calls;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == parent_change_writes &&
              mock.process_held && mock.thread_held,
          "parent change is unknown and preserves the stopped restore lease");
    mock.relationship_valid = 1;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) == 0 &&
              !vdThreadMutationIsActive(&session),
          "watchdog retry restores after the parent relation is re-proven");
}

static void test_release_drain(void)
{
    struct mock_platform mock = make_mock();
    mock.retry_release_call = 1;
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin release-drain fixture");
    check(vdThreadMutationProviderDrain(&provider) ==
              VD_KERNEL_ERROR_MUTATION_BUSY,
          "active retained lease cannot be drained out of band");
    check(vdThreadMutationProviderInit(&provider, &ops) ==
              VD_KERNEL_ERROR_MUTATION_STATE &&
              provider.lease_active && mock.thread_held && mock.process_held,
          "one-shot initialization cannot erase an active lease");
    check(vdThreadMutationRestore(&session, &owner, &handle, backend) == 0 &&
              !vdThreadMutationIsActive(&session) &&
              provider.release_pending && mock.thread_held &&
              mock.process_held &&
              vdThreadMutationSupportedBanks(backend) == 0 &&
              vdThreadMutationProviderLastReleaseError(&provider) ==
                  VD_KERNEL_ERROR_MUTATION_RESTORE,
          "definitely retained release result creates a retryable quarantine");
    mock.retry_release_call = 0;
    check(vdThreadMutationProviderDrain(&provider) == 0 &&
              !mock.thread_held && !mock.process_held &&
              mock.release_order_count == 2u &&
              mock.release_order[0] == 't' &&
              mock.release_order[1] == 'p' &&
              vdThreadMutationProviderReady(&provider) &&
              vdThreadMutationProviderLastReleaseError(&provider) == 0,
          "drain retries references in order and re-enables verified banks");

    mock = make_mock();
    mock.ambiguous_release_call = 1;
    ops = make_ops(&mock);
    provider = (struct vd_thread_mutation_provider){0};
    vdThreadMutationProviderInit(&provider, &ops);
    backend = vdThreadMutationProviderBackend(&provider);
    vdThreadMutationInit(&session);
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0 &&
              vdThreadMutationRestore(&session, &owner, &handle,
                                      backend) == 0,
          "ambiguous-release fixture reaches provider quarantine");
    const int release_calls = mock.release_calls;
    check(provider.release_pending && provider.release_uncertain &&
              !provider.release_retryable && !mock.thread_held &&
              mock.process_held &&
              vdThreadMutationProviderDrain(&provider) == -304 &&
              mock.release_calls == release_calls,
          "unknown release effect is quarantined without a blind retry");
    check(vdThreadMutationProviderInit(&provider, &ops) ==
              VD_KERNEL_ERROR_MUTATION_STATE &&
              provider.release_uncertain && mock.process_held,
          "reinitialization cannot discard an ambiguous cleanup obligation");
}

static void test_disconnect_timeout_watchdog_and_unload(void)
{
    struct mock_platform mock = make_mock();
    struct vd_thread_mutation_provider_ops ops = make_ops(&mock);
    struct vd_thread_mutation_provider provider = {0};
    vdThreadMutationProviderInit(&provider, &ops);
    const struct vd_thread_mutation_backend* backend =
        vdThreadMutationProviderBackend(&provider);
    struct vd_thread_mutation_session session;
    vdThreadMutationInit(&session);
    const struct vd_thread_mutation_identity owner = identity();
    const struct vd_kernel_thread_mutation_begin_request begin =
        begin_request(VD_KERNEL_THREAD_MUTATION_CORE);
    struct vd_kernel_thread_mutation_handle handle;
    const struct vd_thread_registers original = mock.core;
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) == 0,
          "begin disconnect-timeout fixture");
    struct vd_kernel_thread_mutation_write_request write = write_request(
        &handle, VD_KERNEL_THREAD_MUTATION_CORE, 4u, 0x55667788u, 0u);
    check(vdThreadMutationStage(&session, &owner, &write, backend) == 0,
          "stage one callee-saved register before disconnect");

    check(vdThreadMutationProviderPrepareUnload(&provider) ==
              VD_KERNEL_ERROR_MUTATION_BUSY &&
              !vdThreadMutationProviderReady(&provider),
          "plugin unload blocks admission and waits for the active lease");
    mock.target_state = -1;
    const int writes_before = mock.write_core_calls;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE &&
              vdThreadMutationIsActive(&session) &&
              mock.write_core_calls == writes_before,
          "disconnect timeout with failed inventory keeps the target stopped");
    mock.target_state = 1;
    mock.retry_release_call = mock.release_calls + 1;
    check(vdThreadMutationCleanup(&session, owner.owner_pid,
                                  owner.stop_token, backend) == 0 &&
              core_same(&mock.core, &original) &&
              !vdThreadMutationIsActive(&session) &&
              provider.release_pending && mock.thread_held &&
              mock.process_held,
          "watchdog restores exactly before quarantining a retained release");
    check(vdThreadMutationProviderPrepareUnload(&provider) ==
              VD_KERNEL_ERROR_MUTATION_RESTORE,
          "unload remains blocked while retained cleanup is pending");
    mock.retry_release_call = 0;
    check(vdThreadMutationProviderDrain(&provider) == 0 &&
              !mock.thread_held && !mock.process_held &&
              vdThreadMutationProviderPrepareUnload(&provider) == 0 &&
              !vdThreadMutationProviderReady(&provider) &&
              vdThreadMutationSupportedBanks(backend) == 0,
          "watchdog drain permits unload without re-enabling writes");
    check(vdThreadMutationBegin(&session, &owner, &begin, backend,
                                &handle) ==
              VD_KERNEL_ERROR_MUTATION_UNSUPPORTED,
          "terminal unload state rejects reconnect transactions");
}

int main(void)
{
    test_binding_gate();
    test_exact_reference_core_and_vfp();
    test_partial_retain_unwind();
    test_failed_retain_contract_quarantine();
    test_rollback_and_transient_liveness();
    test_target_exit_and_unknown_state();
    test_release_drain();
    test_disconnect_timeout_watchdog_and_unload();
    if(failures)
        return 1;
    puts("PASS: verified-setter retained-target provider lifecycle");
    return 0;
}
