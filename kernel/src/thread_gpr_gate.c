#include "thread_gpr_gate.h"

#define VD_THREAD_GPR_GATE_INITIALIZED UINT32_C(0x47505231)

static void zero_bytes(void* value, unsigned int size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)value;
    for(unsigned int i = 0; i < size; ++i)
        bytes[i] = 0;
}

static int bytes_are_zero(const void* value, unsigned int size)
{
    const volatile unsigned char* bytes =
        (const volatile unsigned char*)value;
    for(unsigned int i = 0; i < size; ++i)
        if(bytes[i] != 0)
            return 0;
    return 1;
}

static int register_allowed(unsigned int register_index)
{
    return register_index == VD_THREAD_GPR_GATE_REGISTER_R4 ||
           register_index == VD_THREAD_GPR_GATE_REGISTER_R5;
}

static void clear_gate_metadata(struct vd_thread_gpr_gate* gate)
{
    zero_bytes(&gate->handle, sizeof(gate->handle));
    gate->provider = 0;
    gate->register_index = 0;
    gate->register_bank = 0;
}

static int finish_operation(
    struct vd_thread_gpr_gate* gate, int operation_result)
{
    if(vdThreadMutationIsActive(&gate->transaction))
        return operation_result;
    const int release_result = gate->provider ?
        vdThreadMutationProviderLastReleaseError(gate->provider) : 0;
    clear_gate_metadata(gate);
    return release_result < 0 ? release_result : operation_result;
}

int vdThreadGprGateInit(struct vd_thread_gpr_gate* gate)
{
    if(!gate)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(!bytes_are_zero(gate, sizeof(*gate)))
        return VD_KERNEL_ERROR_MUTATION_STATE;
    zero_bytes(gate, sizeof(*gate));
    vdThreadMutationInit(&gate->transaction);
    gate->initialized = VD_THREAD_GPR_GATE_INITIALIZED;
    return 0;
}

int vdThreadGprGateCompiled(void)
{
    return VD_THREAD_GPR_GATE_COMPILED ? 1 : 0;
}

int vdThreadGprGateBegin(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity,
    unsigned int register_index,
    struct vd_thread_mutation_provider* provider)
{
    if(!VD_THREAD_GPR_GATE_COMPILED)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
    if(!gate || gate->initialized != VD_THREAD_GPR_GATE_INITIALIZED ||
       !identity || !provider || !register_allowed(register_index))
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(vdThreadMutationIsActive(&gate->transaction) || gate->provider)
        return VD_KERNEL_ERROR_MUTATION_BUSY;
    if(!vdThreadMutationProviderReady(provider))
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;

    struct vd_thread_core_setter_contract contract;
    int result = vdThreadMutationProviderCoreContract(provider, &contract);
    if(result < 0)
        return result;
    if((contract.writable_gpr_mask & (1u << register_index)) == 0)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;

    const struct vd_kernel_thread_mutation_begin_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_THREAD_MUTATION_ABI_VERSION,
        .stop_token = identity->stop_token,
        .target_thread = identity->target_user_thread,
        .bank_mask = VD_KERNEL_THREAD_MUTATION_CORE,
        .flags = 0,
    };
    result = vdThreadMutationBegin(
        &gate->transaction, identity, &request,
        vdThreadMutationProviderBackend(provider), &gate->handle);
    if(result < 0)
        return result;

    gate->provider = provider;
    gate->register_index = register_index;
    result = vdThreadMutationGetSelectedUserCoreBank(
        &gate->transaction, &gate->register_bank);
    if(result < 0)
    {
        const int restore_result = vdThreadMutationRestore(
            &gate->transaction, identity, &gate->handle,
            vdThreadMutationProviderBackend(provider));
        const int cleanup_result = finish_operation(
            gate, restore_result);
        return cleanup_result < 0 ? cleanup_result : result;
    }
    return 0;
}

int vdThreadGprGateApply(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity,
    unsigned int value)
{
    if(!VD_THREAD_GPR_GATE_COMPILED)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
    if(!gate || gate->initialized != VD_THREAD_GPR_GATE_INITIALIZED ||
       !identity || !gate->provider ||
       !vdThreadMutationIsActive(&gate->transaction))
        return VD_KERNEL_ERROR_MUTATION_STATE;
    if(gate->transaction.state != VD_THREAD_MUTATION_SNAPSHOTTED)
        return VD_KERNEL_ERROR_MUTATION_STATE;

    const struct vd_kernel_thread_mutation_write_request request = {
        .handle = gate->handle,
        .bank = VD_KERNEL_THREAD_MUTATION_CORE,
        .register_bank = gate->register_bank,
        .register_index = gate->register_index,
        .value_low = value,
        .value_high = 0,
        .flags = 0,
    };
    const int result = vdThreadMutationStage(
        &gate->transaction, identity, &request,
        vdThreadMutationProviderBackend(gate->provider));
    return finish_operation(gate, result);
}

int vdThreadGprGateRestore(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity)
{
    if(!VD_THREAD_GPR_GATE_COMPILED)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
    if(!gate || gate->initialized != VD_THREAD_GPR_GATE_INITIALIZED ||
       !identity || !gate->provider ||
       !vdThreadMutationIsActive(&gate->transaction))
        return VD_KERNEL_ERROR_MUTATION_STATE;
    const int result = vdThreadMutationRestore(
        &gate->transaction, identity, &gate->handle,
        vdThreadMutationProviderBackend(gate->provider));
    return finish_operation(gate, result);
}

int vdThreadGprGateCleanup(
    struct vd_thread_gpr_gate* gate,
    SceUID owner_pid,
    unsigned int stop_token)
{
    if(!VD_THREAD_GPR_GATE_COMPILED)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
    if(!gate || gate->initialized != VD_THREAD_GPR_GATE_INITIALIZED)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(!vdThreadMutationIsActive(&gate->transaction))
    {
        clear_gate_metadata(gate);
        return 0;
    }
    if(!gate->provider)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    const int result = vdThreadMutationCleanup(
        &gate->transaction, owner_pid, stop_token,
        vdThreadMutationProviderBackend(gate->provider));
    return finish_operation(gate, result);
}

int vdThreadGprGateIsActive(const struct vd_thread_gpr_gate* gate)
{
    return gate &&
           gate->initialized == VD_THREAD_GPR_GATE_INITIALIZED &&
           vdThreadMutationIsActive(&gate->transaction);
}
