#include "thread_mutation_provider.h"

#define VD_THREAD_MUTATION_PROVIDER_ALLOWED_BANKS \
    (VD_KERNEL_THREAD_MUTATION_CORE | VD_KERNEL_THREAD_MUTATION_VFP)

static void zero_bytes(void* value, unsigned int size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)value;
    for(unsigned int i = 0; i < size; ++i)
        bytes[i] = 0;
}

static void copy_bytes(void* destination, const void* source,
                       unsigned int size)
{
    volatile unsigned char* output =
        (volatile unsigned char*)destination;
    const volatile unsigned char* input =
        (const volatile unsigned char*)source;
    for(unsigned int i = 0; i < size; ++i)
        output[i] = input[i];
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

static int identity_equal(
    const struct vd_thread_mutation_identity* left,
    const struct vd_thread_mutation_identity* right)
{
    return left && right && left->owner_pid == right->owner_pid &&
           left->owner_thread == right->owner_thread &&
           left->stop_token == right->stop_token &&
           left->target_user_thread == right->target_user_thread &&
           left->target_guid == right->target_guid;
}

static int reference_valid(
    const struct vd_thread_target_reference* reference,
    SceUID expected_guid)
{
    return reference && reference->guid == expected_guid &&
           reference->guid >= 0 && reference->object != 0 &&
           reference->generation != 0;
}

static int binding_valid(
    const struct vd_thread_mutation_provider_ops* ops,
    const struct vd_thread_setter_binding* binding)
{
    if(!ops || !binding ||
       binding->struct_size != sizeof(*binding) ||
       binding->version != VD_THREAD_SETTER_BINDING_VERSION ||
       binding->module_id < 0 || binding->module_nid == 0 ||
       binding->supported_banks == 0 ||
       (binding->supported_banks &
        ~VD_THREAD_MUTATION_PROVIDER_ALLOWED_BANKS) != 0 ||
       !bytes_are_zero(binding->reserved, sizeof(binding->reserved)) ||
       bytes_are_zero(binding->fingerprint,
                      sizeof(binding->fingerprint)))
        return 0;

    if((binding->supported_banks & VD_KERNEL_THREAD_MUTATION_CORE) != 0 &&
       (binding->core_get == 0 || binding->core_set == 0 ||
        !ops->snapshot_core || !ops->write_core))
        return 0;
    if((binding->supported_banks & VD_KERNEL_THREAD_MUTATION_VFP) != 0 &&
       (binding->vfp_get == 0 || binding->vfp_set == 0 ||
        !ops->snapshot_vfp || !ops->write_vfp))
        return 0;
    return 1;
}

static void disable_provider(struct vd_thread_mutation_provider* provider)
{
    if(provider)
        provider->backend.supported_banks = 0;
}

static int binding_is_live(
    struct vd_thread_mutation_provider* provider)
{
    if(!provider || !provider->binding_ready ||
       !provider->ops.binding_status)
        return 0;
    int result = provider->ops.binding_status(
        provider->ops.context, &provider->binding);
    if(result == 1)
    {
        if(!provider->release_pending)
            provider->backend.supported_banks =
                provider->configured_banks;
        return 1;
    }
    else
    {
        disable_provider(provider);
        return 0;
    }
}

static int release_retained_references(
    struct vd_thread_mutation_provider* provider)
{
    if(!provider || !provider->ops.release_reference)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;

    int result;
    if(provider->thread_retained)
    {
        result = provider->ops.release_reference(
            provider->ops.context, &provider->thread);
        if(result != VD_THREAD_REFERENCE_RELEASED)
        {
            provider->release_pending = 1;
            provider->release_retryable =
                result == VD_THREAD_REFERENCE_RELEASE_RETAINED;
            provider->release_uncertain = !provider->release_retryable;
            provider->last_release_error =
                result < 0 ? result : VD_KERNEL_ERROR_MUTATION_RESTORE;
            disable_provider(provider);
            return provider->last_release_error;
        }
        provider->thread_retained = 0;
        zero_bytes(&provider->thread, sizeof(provider->thread));
    }
    if(provider->process_retained)
    {
        result = provider->ops.release_reference(
            provider->ops.context, &provider->process);
        if(result != VD_THREAD_REFERENCE_RELEASED)
        {
            provider->release_pending = 1;
            provider->release_retryable =
                result == VD_THREAD_REFERENCE_RELEASE_RETAINED;
            provider->release_uncertain = !provider->release_retryable;
            provider->last_release_error =
                result < 0 ? result : VD_KERNEL_ERROR_MUTATION_RESTORE;
            disable_provider(provider);
            return provider->last_release_error;
        }
        provider->process_retained = 0;
        zero_bytes(&provider->process, sizeof(provider->process));
    }

    provider->release_pending = 0;
    provider->release_retryable = 0;
    provider->release_uncertain = 0;
    provider->last_release_error = 0;
    if(provider->binding_ready && binding_is_live(provider))
        provider->backend.supported_banks = provider->configured_banks;
    return 0;
}

static int provider_retain_target(
    void* context,
    const struct vd_thread_mutation_identity* identity)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !identity || provider->lease_active ||
       provider->release_pending || provider->process_retained ||
       provider->thread_retained || !binding_is_live(provider) ||
       !provider->ops.retain_process || !provider->ops.retain_thread ||
       !provider->ops.retained_target_status)
        return VD_KERNEL_ERROR_MUTATION_TARGET;

    copy_bytes(&provider->identity, identity, sizeof(provider->identity));
    zero_bytes(&provider->process, sizeof(provider->process));
    zero_bytes(&provider->thread, sizeof(provider->thread));

    int result = provider->ops.retain_process(
        provider->ops.context, identity->owner_pid, &provider->process);
    if(result < 0)
        return result;
    provider->process_retained = 1;
    if(!reference_valid(&provider->process, identity->owner_pid))
        return VD_KERNEL_ERROR_MUTATION_TARGET;

    result = provider->ops.retain_thread(
        provider->ops.context, identity->target_guid, &provider->thread);
    if(result < 0)
        return result;
    provider->thread_retained = 1;
    if(!reference_valid(&provider->thread, identity->target_guid) ||
       provider->thread.object == provider->process.object)
        return VD_KERNEL_ERROR_MUTATION_TARGET;

    provider->lease_active = 1;
    result = provider->ops.retained_target_status(
        provider->ops.context, &provider->process, &provider->thread,
        identity);
    if(result != 1)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    return 0;
}

static void provider_release_target(
    void* context,
    const struct vd_thread_mutation_identity* identity)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider)
        return;
    if(provider->lease_active &&
       !identity_equal(&provider->identity, identity))
    {
        provider->release_pending = 1;
        provider->release_uncertain = 1;
        provider->release_retryable = 0;
        provider->last_release_error = VD_KERNEL_ERROR_MUTATION_OWNER;
        disable_provider(provider);
        return;
    }
    provider->lease_active = 0;
    zero_bytes(&provider->identity, sizeof(provider->identity));
    (void)release_retained_references(provider);
}

static int provider_target_status(
    void* context,
    const struct vd_thread_mutation_identity* identity)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !provider->lease_active ||
       !provider->process_retained || !provider->thread_retained ||
       !identity_equal(&provider->identity, identity) ||
       !binding_is_live(provider))
        return -1;
    int result = provider->ops.retained_target_status(
        provider->ops.context, &provider->process, &provider->thread,
        identity);
    if(result == 1)
        return result;
    disable_provider(provider);
    return result == 0 ? 0 : -1;
}

static int begin_target_access(
    struct vd_thread_mutation_provider* provider)
{
    if(!provider || !provider->ops.begin_target_access ||
       !provider->ops.end_target_access ||
       provider_target_status(provider, &provider->identity) != 1)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    int result = provider->ops.begin_target_access(
        provider->ops.context, &provider->binding, &provider->process,
        &provider->thread, &provider->identity);
    if(result != 1)
    {
        disable_provider(provider);
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    }
    return 0;
}

static int provider_snapshot_core(
    void* context, SceUID target_guid,
    struct vd_thread_registers* registers)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !registers || !provider->ops.snapshot_core ||
       target_guid != provider->thread.guid ||
       begin_target_access(provider) < 0)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    int result = provider->ops.snapshot_core(
        provider->ops.context, &provider->binding, &provider->thread,
        registers);
    provider->ops.end_target_access(provider->ops.context);
    return result;
}

static int provider_write_core(
    void* context, SceUID target_guid,
    const struct vd_thread_registers* registers)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !registers || !provider->ops.write_core ||
       target_guid != provider->thread.guid ||
       begin_target_access(provider) < 0)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    int result = provider->ops.write_core(
        provider->ops.context, &provider->binding, &provider->thread,
        registers);
    provider->ops.end_target_access(provider->ops.context);
    return result;
}

static int provider_snapshot_vfp(
    void* context, SceUID target_guid,
    struct vd_thread_vfp_registers* registers)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !registers || !provider->ops.snapshot_vfp ||
       target_guid != provider->thread.guid ||
       begin_target_access(provider) < 0)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    int result = provider->ops.snapshot_vfp(
        provider->ops.context, &provider->binding, &provider->thread,
        registers);
    provider->ops.end_target_access(provider->ops.context);
    return result;
}

static int provider_write_vfp(
    void* context, SceUID target_guid,
    const struct vd_thread_vfp_registers* registers)
{
    struct vd_thread_mutation_provider* provider =
        (struct vd_thread_mutation_provider*)context;
    if(!provider || !registers || !provider->ops.write_vfp ||
       target_guid != provider->thread.guid ||
       begin_target_access(provider) < 0)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    int result = provider->ops.write_vfp(
        provider->ops.context, &provider->binding, &provider->thread,
        registers);
    provider->ops.end_target_access(provider->ops.context);
    return result;
}

int vdThreadMutationProviderInit(
    struct vd_thread_mutation_provider* provider,
    const struct vd_thread_mutation_provider_ops* ops)
{
    if(!provider)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(!bytes_are_zero(provider, sizeof(*provider)))
        return VD_KERNEL_ERROR_MUTATION_STATE;
    zero_bytes(provider, sizeof(*provider));
    provider->backend.context = provider;
    provider->backend.retain_target = provider_retain_target;
    provider->backend.release_target = provider_release_target;
    provider->backend.retained_target_status = provider_target_status;
    provider->backend.snapshot_core = provider_snapshot_core;
    provider->backend.write_core = provider_write_core;
    provider->backend.snapshot_vfp = provider_snapshot_vfp;
    provider->backend.write_vfp = provider_write_vfp;
    if(!ops)
        return 0;
    copy_bytes(&provider->ops, ops, sizeof(provider->ops));
    if(!provider->ops.resolve_verified_binding ||
       !provider->ops.binding_status || !provider->ops.retain_process ||
       !provider->ops.retain_thread ||
       !provider->ops.release_reference ||
       !provider->ops.retained_target_status ||
       !provider->ops.begin_target_access ||
       !provider->ops.end_target_access)
        return 0;

    struct vd_thread_setter_binding binding;
    zero_bytes(&binding, sizeof(binding));
    if(provider->ops.resolve_verified_binding(
           provider->ops.context, &binding) < 0 ||
       !binding_valid(&provider->ops, &binding))
        return 0;
    copy_bytes(&provider->binding, &binding, sizeof(provider->binding));
    provider->configured_banks = binding.supported_banks;
    provider->binding_ready = 1;
    if(binding_is_live(provider))
        provider->backend.supported_banks = provider->configured_banks;
    return 0;
}

const struct vd_thread_mutation_backend* vdThreadMutationProviderBackend(
    struct vd_thread_mutation_provider* provider)
{
    return provider ? &provider->backend : 0;
}

int vdThreadMutationProviderDrain(
    struct vd_thread_mutation_provider* provider)
{
    if(!provider)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(provider->lease_active)
        return VD_KERNEL_ERROR_MUTATION_BUSY;
    if(!provider->release_pending && !provider->process_retained &&
       !provider->thread_retained)
        return 0;
    if(provider->release_uncertain)
        return provider->last_release_error ?
            provider->last_release_error : VD_KERNEL_ERROR_MUTATION_RESTORE;
    if(!provider->release_retryable)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    return release_retained_references(provider);
}

int vdThreadMutationProviderReady(
    struct vd_thread_mutation_provider* provider)
{
    return provider && provider->binding_ready &&
           !provider->lease_active && !provider->release_pending &&
           !provider->process_retained && !provider->thread_retained &&
           binding_is_live(provider) &&
           provider->backend.supported_banks != 0;
}

int vdThreadMutationProviderLastReleaseError(
    const struct vd_thread_mutation_provider* provider)
{
    return provider ? provider->last_release_error :
                      VD_KERNEL_ERROR_MUTATION_INVALID;
}
