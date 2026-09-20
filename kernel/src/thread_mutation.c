#include "thread_mutation.h"

#define VD_THREAD_MUTATION_ALLOWED_BANKS \
    (VD_KERNEL_THREAD_MUTATION_CORE | VD_KERNEL_THREAD_MUTATION_VFP)
#define VD_THREAD_MUTATION_CORE_REGISTER_MASK \
    ((1u << VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT) - 1u)
#define VD_ARM_CPSR_T (1u << 5)
#define VD_ARM_CPSR_MODE_MASK 0x1fu
#define VD_ARM_CPSR_MODE_USER 0x10u

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

static void clear_transaction(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_backend* backend)
{
    uint64_t next_sequence = session->next_sequence;
    if(session->target_retained && backend && backend->release_target)
        backend->release_target(backend->context, &session->identity);
    zero_bytes(session, sizeof(*session));
    session->next_sequence = next_sequence ? next_sequence : UINT64_C(1);
}

static void take_handle_sequence(struct vd_thread_mutation_session* session)
{
    uint64_t sequence = session->next_sequence;
    if(sequence == 0)
        sequence = UINT64_C(1);
    while((unsigned int)sequence == 0)
        sequence++;
    session->token = (unsigned int)sequence;
    session->generation = (unsigned int)(sequence >> 32) + 1u;
    if(session->generation == 0)
        session->generation = 1u;
    session->next_sequence = sequence + 1u;
    if(session->next_sequence == 0)
        session->next_sequence = UINT64_C(1);
}

static int identity_valid(const struct vd_thread_mutation_identity* identity)
{
    return identity && identity->owner_pid >= 0 &&
           identity->owner_thread >= 0 && identity->stop_token != 0 &&
           identity->target_user_thread >= 0 && identity->target_guid >= 0 &&
           identity->owner_thread != identity->target_guid;
}

static int identity_equal(
    const struct vd_thread_mutation_identity* left,
    const struct vd_thread_mutation_identity* right)
{
    return left->owner_pid == right->owner_pid &&
           left->owner_thread == right->owner_thread &&
           left->stop_token == right->stop_token &&
           left->target_user_thread == right->target_user_thread &&
           left->target_guid == right->target_guid;
}

static int handle_valid(
    const struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_handle* handle)
{
    if(!session || !identity || !handle ||
       session->state == VD_THREAD_MUTATION_EMPTY)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    if(handle->struct_size != sizeof(*handle) ||
       handle->abi_version != VD_KERNEL_THREAD_MUTATION_ABI_VERSION ||
       handle->reserved != 0 || handle->token == 0 ||
       handle->generation == 0 || handle->stop_token == 0)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(!identity_equal(&session->identity, identity) ||
       handle->token != session->token ||
       handle->generation != session->generation ||
       handle->stop_token != session->identity.stop_token ||
       handle->target_thread != session->identity.target_user_thread ||
       handle->bank_mask != session->bank_mask)
        return VD_KERNEL_ERROR_MUTATION_OWNER;
    return 0;
}

static int core_equal(const struct vd_thread_registers* left,
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

static int vfp_layout_valid(const struct vd_thread_vfp_registers* registers)
{
    return registers &&
           registers->layout_version == VD_KERNEL_VFP_LAYOUT_D32_V1 &&
           registers->d_register_count == VD_KERNEL_VFP_D_REGISTER_COUNT;
}

static int vfp_equal(const struct vd_thread_vfp_registers* left,
                     const struct vd_thread_vfp_registers* right)
{
    if(!vfp_layout_valid(left) || !vfp_layout_valid(right))
        return 0;
    for(unsigned int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
        if(left->d[i] != right->d[i])
            return 0;
    for(unsigned int i = 0; i < 2u; ++i)
        if(left->fpscr_entry[i] != right->fpscr_entry[i])
            return 0;
    return 1;
}

static unsigned int* core_register(
    struct vd_thread_registers* registers,
    unsigned int bank,
    unsigned int index)
{
    if(bank >= VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT ||
       index >= VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT)
        return 0;
    if(index < 13u)
        return &registers->entry[bank].r[index];
    if(index == 13u)
        return &registers->entry[bank].sp;
    if(index == 14u)
        return &registers->entry[bank].lr;
    if(index == 15u)
        return &registers->entry[bank].pc;
    return &registers->entry[bank].cpsr;
}

static int selected_user_core_bank(
    const struct vd_thread_registers* registers)
{
    for(unsigned int bank = 0;
        bank < VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT; ++bank)
    {
        const struct vd_arm_registers* candidate = &registers->entry[bank];
        if((candidate->cpsr & VD_ARM_CPSR_MODE_MASK) ==
               VD_ARM_CPSR_MODE_USER &&
           candidate->pc != 0 && candidate->sp != 0)
            return (int)bank;
    }
    return -1;
}

static int desired_core_valid(
    const struct vd_thread_mutation_session* session)
{
    for(unsigned int bank = 0;
        bank < VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT; ++bank)
    {
        const struct vd_arm_registers* original =
            &session->original_core.entry[bank];
        const struct vd_arm_registers* desired =
            &session->desired_core.entry[bank];
        if(((original->cpsr ^ desired->cpsr) &
            ~VD_KERNEL_THREAD_MUTATION_CPSR_WRITABLE_MASK) != 0 ||
           original->fpscr != desired->fpscr)
            return 0;
        if((desired->cpsr & VD_ARM_CPSR_T) != 0)
        {
            if((desired->pc & 1u) != 0)
                return 0;
        }
        else if((desired->pc & 3u) != 0)
        {
            return 0;
        }
    }
    return 1;
}

void vdThreadMutationInit(struct vd_thread_mutation_session* session)
{
    if(!session)
        return;
    zero_bytes(session, sizeof(*session));
    session->next_sequence = UINT64_C(1);
}

unsigned int vdThreadMutationSupportedBanks(
    const struct vd_thread_mutation_backend* backend)
{
    if(!backend)
        return 0;
    unsigned int supported = backend->supported_banks &
                             VD_THREAD_MUTATION_ALLOWED_BANKS;
    if(!backend->retain_target || !backend->release_target ||
       !backend->retained_target_status)
        return 0;
    if(!backend->snapshot_core || !backend->write_core ||
       backend->core_writable_register_mask == 0 ||
       (backend->core_writable_register_mask &
        ~VD_THREAD_MUTATION_CORE_REGISTER_MASK) != 0)
        supported &= ~VD_KERNEL_THREAD_MUTATION_CORE;
    if(!backend->snapshot_vfp || !backend->write_vfp)
        supported &= ~VD_KERNEL_THREAD_MUTATION_VFP;
    return supported;
}

int vdThreadMutationBegin(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_begin_request* request,
    const struct vd_thread_mutation_backend* backend,
    struct vd_kernel_thread_mutation_handle* handle)
{
    if(!session || !identity_valid(identity) || !request || !handle ||
       request->struct_size != sizeof(*request) ||
       request->abi_version != VD_KERNEL_THREAD_MUTATION_ABI_VERSION ||
       request->stop_token != identity->stop_token ||
       request->target_thread != identity->target_user_thread ||
       (request->bank_mask != VD_KERNEL_THREAD_MUTATION_CORE &&
        request->bank_mask != VD_KERNEL_THREAD_MUTATION_VFP) ||
       (request->bank_mask & ~VD_THREAD_MUTATION_ALLOWED_BANKS) != 0 ||
       request->flags != 0)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if(session->state != VD_THREAD_MUTATION_EMPTY)
        return VD_KERNEL_ERROR_MUTATION_BUSY;

    unsigned int supported = vdThreadMutationSupportedBanks(backend);
    if((request->bank_mask & supported) != request->bank_mask)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;

    int result;
    copy_bytes(&session->identity, identity, sizeof(session->identity));
    // Mark the attempt first: a backend may retain the process object and then
    // fail while retaining the thread. Its idempotent release callback must be
    // given a chance to unwind any partial acquisition.
    session->target_retained = 1;
    result = backend->retain_target(backend->context, identity);
    if(result < 0)
    {
        clear_transaction(session, backend);
        return result;
    }
    if(backend->retained_target_status(backend->context, identity) != 1)
    {
        clear_transaction(session, backend);
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    }

    if((request->bank_mask & VD_KERNEL_THREAD_MUTATION_CORE) != 0)
    {
        result = backend->snapshot_core(backend->context,
                                        identity->target_guid,
                                        &session->original_core);
        if(result < 0)
        {
            clear_transaction(session, backend);
            return result;
        }
        copy_bytes(&session->desired_core, &session->original_core,
                   sizeof(session->desired_core));
        if(!desired_core_valid(session) ||
           selected_user_core_bank(&session->original_core) < 0)
        {
            clear_transaction(session, backend);
            return VD_KERNEL_ERROR_MUTATION_TARGET;
        }
    }
    if((request->bank_mask & VD_KERNEL_THREAD_MUTATION_VFP) != 0)
    {
        result = backend->snapshot_vfp(backend->context,
                                       identity->target_guid,
                                       &session->original_vfp);
        if(result < 0)
        {
            clear_transaction(session, backend);
            return result;
        }
        if(!vfp_layout_valid(&session->original_vfp))
        {
            clear_transaction(session, backend);
            return VD_KERNEL_ERROR_MUTATION_VERIFY;
        }
        copy_bytes(&session->desired_vfp, &session->original_vfp,
                   sizeof(session->desired_vfp));
    }

    session->bank_mask = request->bank_mask;
    session->staged_banks = 0;
    take_handle_sequence(session);
    session->state = VD_THREAD_MUTATION_SNAPSHOTTED;

    const struct vd_kernel_thread_mutation_handle kernel_handle = {
        .struct_size = sizeof(kernel_handle),
        .abi_version = VD_KERNEL_THREAD_MUTATION_ABI_VERSION,
        .token = session->token,
        .generation = session->generation,
        .stop_token = identity->stop_token,
        .target_thread = identity->target_user_thread,
        .bank_mask = request->bank_mask,
        .reserved = 0,
    };
    copy_bytes(handle, &kernel_handle, sizeof(*handle));
    return 0;
}

static int restore_exact(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_backend* backend);

static int rollback_after_error(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_backend* backend,
    int operation_error);

int vdThreadMutationStage(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_write_request* request,
    const struct vd_thread_mutation_backend* backend)
{
    if(!request)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    int result = handle_valid(session, identity, &request->handle);
    if(result < 0)
        return result;
    if(session->state != VD_THREAD_MUTATION_SNAPSHOTTED &&
       session->state != VD_THREAD_MUTATION_STAGED)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    if(request->flags != 0 ||
       (request->bank != VD_KERNEL_THREAD_MUTATION_CORE &&
        request->bank != VD_KERNEL_THREAD_MUTATION_VFP) ||
       (session->bank_mask & request->bank) == 0)
        return VD_KERNEL_ERROR_MUTATION_INVALID;
    if((vdThreadMutationSupportedBanks(backend) & request->bank) == 0)
        return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
    if(!session->target_retained ||
       backend->retained_target_status(backend->context, identity) != 1)
        return VD_KERNEL_ERROR_MUTATION_TARGET;

    if(request->bank == VD_KERNEL_THREAD_MUTATION_CORE)
    {
        if(request->value_high != 0 ||
           request->register_bank >=
               VD_KERNEL_THREAD_MUTATION_CORE_BANK_COUNT ||
           request->register_index >=
               VD_KERNEL_THREAD_MUTATION_CORE_REGISTER_COUNT)
            return VD_KERNEL_ERROR_MUTATION_INVALID;
        if((backend->core_writable_register_mask &
            (1u << request->register_index)) == 0)
            return VD_KERNEL_ERROR_MUTATION_UNSUPPORTED;
        if((int)request->register_bank !=
           selected_user_core_bank(&session->original_core))
            return VD_KERNEL_ERROR_MUTATION_TARGET;
        unsigned int* destination = core_register(
            &session->desired_core, request->register_bank,
            request->register_index);
        if(!destination)
            return VD_KERNEL_ERROR_MUTATION_INVALID;
        if(request->register_index == 16u)
        {
            const unsigned int original =
                session->original_core.entry[request->register_bank].cpsr;
            if(((original ^ request->value_low) &
                ~VD_KERNEL_THREAD_MUTATION_CPSR_WRITABLE_MASK) != 0)
                return VD_KERNEL_ERROR_MUTATION_INVALID;
        }
        unsigned int previous = *destination;
        *destination = request->value_low;
        if(!desired_core_valid(session))
        {
            *destination = previous;
            return VD_KERNEL_ERROR_MUTATION_INVALID;
        }
    }
    else
    {
        if(request->register_bank != 0 ||
           request->register_index >=
               VD_KERNEL_THREAD_MUTATION_VFP_REGISTER_COUNT)
            return VD_KERNEL_ERROR_MUTATION_INVALID;
        if(request->register_index < VD_KERNEL_VFP_D_REGISTER_COUNT)
        {
            session->desired_vfp.d[request->register_index] =
                ((uint64_t)request->value_high << 32) |
                (uint64_t)request->value_low;
        }
        else
        {
            if(request->value_high != 0)
                return VD_KERNEL_ERROR_MUTATION_INVALID;
            // The validated D32 v1 layout identifies raw entry 0 as FPSCR.
            // Preserve entry 1 as snapshot evidence rather than guessing that
            // both state-dependent CPU banks should be changed together.
            session->desired_vfp.fpscr_entry[
                VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1] = request->value_low;
        }
    }
    session->staged_banks |= request->bank;
    session->state = VD_THREAD_MUTATION_RESTORE_PENDING;

    if(request->bank == VD_KERNEL_THREAD_MUTATION_CORE)
    {
        result = backend->write_core(backend->context,
                                     identity->target_guid,
                                     &session->desired_core);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        struct vd_thread_registers actual;
        result = backend->snapshot_core(backend->context,
                                         identity->target_guid, &actual);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        if(!core_equal(&actual, &session->desired_core))
            return rollback_after_error(
                session, backend, VD_KERNEL_ERROR_MUTATION_VERIFY);
    }
    else
    {
        result = backend->write_vfp(backend->context,
                                    identity->target_guid,
                                    &session->desired_vfp);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        struct vd_thread_vfp_registers actual;
        result = backend->snapshot_vfp(backend->context,
                                        identity->target_guid, &actual);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        if(!vfp_equal(&actual, &session->desired_vfp))
            return rollback_after_error(
                session, backend, VD_KERNEL_ERROR_MUTATION_VERIFY);
    }

    session->state = VD_THREAD_MUTATION_STAGED;
    return 0;
}

static int restore_exact(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_backend* backend)
{
    int first_error = 0;
    session->state = VD_THREAD_MUTATION_RESTORE_PENDING;
    if(!session->target_retained || !backend ||
       !backend->retained_target_status ||
       backend->retained_target_status(backend->context,
                                       &session->identity) != 1)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    if((vdThreadMutationSupportedBanks(backend) & session->staged_banks) !=
       session->staged_banks)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;

    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_VFP) != 0)
    {
        int result = backend->write_vfp(backend->context,
                                        session->identity.target_guid,
                                        &session->original_vfp);
        if(result < 0 && first_error == 0)
            first_error = result;
    }
    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_CORE) != 0)
    {
        int result = backend->write_core(backend->context,
                                         session->identity.target_guid,
                                         &session->original_core);
        if(result < 0 && first_error == 0)
            first_error = result;
    }

    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_CORE) != 0)
    {
        struct vd_thread_registers actual;
        int result = backend->snapshot_core(backend->context,
                                             session->identity.target_guid,
                                             &actual);
        if(result < 0 && first_error == 0)
            first_error = result;
        else if(result >= 0 && !core_equal(&actual,
                                           &session->original_core) &&
                first_error == 0)
            first_error = VD_KERNEL_ERROR_MUTATION_VERIFY;
    }
    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_VFP) != 0)
    {
        struct vd_thread_vfp_registers actual;
        int result = backend->snapshot_vfp(backend->context,
                                            session->identity.target_guid,
                                            &actual);
        if(result < 0 && first_error == 0)
            first_error = result;
        else if(result >= 0 && !vfp_equal(&actual,
                                          &session->original_vfp) &&
                first_error == 0)
            first_error = VD_KERNEL_ERROR_MUTATION_VERIFY;
    }

    if(first_error < 0)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    clear_transaction(session, backend);
    return 0;
}

static int rollback_after_error(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_backend* backend,
    int operation_error)
{
    if(restore_exact(session, backend) < 0)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    return operation_error;
}

int vdThreadMutationCommit(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_handle* handle,
    const struct vd_thread_mutation_backend* backend)
{
    int result = handle_valid(session, identity, handle);
    if(result < 0)
        return result;
    if(session->state != VD_THREAD_MUTATION_STAGED ||
       session->staged_banks != session->bank_mask ||
       (vdThreadMutationSupportedBanks(backend) & session->bank_mask) !=
           session->bank_mask)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    if(!session->target_retained ||
       backend->retained_target_status(backend->context, identity) != 1)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_CORE) != 0)
    {
        struct vd_thread_registers actual;
        result = backend->snapshot_core(backend->context,
                                         identity->target_guid, &actual);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        if(!core_equal(&actual, &session->desired_core))
            return rollback_after_error(
                session, backend, VD_KERNEL_ERROR_MUTATION_VERIFY);
    }
    if((session->staged_banks & VD_KERNEL_THREAD_MUTATION_VFP) != 0)
    {
        struct vd_thread_vfp_registers actual;
        result = backend->snapshot_vfp(backend->context,
                                        identity->target_guid, &actual);
        if(result < 0)
            return rollback_after_error(session, backend, result);
        if(!vfp_equal(&actual, &session->desired_vfp))
            return rollback_after_error(
                session, backend, VD_KERNEL_ERROR_MUTATION_VERIFY);
    }

    // Commit is the sole path that accepts provisional target state. Retiring
    // the snapshot here allows EndStop to resume the changed register bank;
    // every pre-commit exit path remains restoration-protected.
    clear_transaction(session, backend);
    return 0;
}

int vdThreadMutationRestore(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_handle* handle,
    const struct vd_thread_mutation_backend* backend)
{
    int result = handle_valid(session, identity, handle);
    if(result < 0)
        return result;
    if(session->state == VD_THREAD_MUTATION_SNAPSHOTTED)
    {
        clear_transaction(session, backend);
        return 0;
    }
    if(session->state != VD_THREAD_MUTATION_STAGED &&
       session->state != VD_THREAD_MUTATION_RESTORE_PENDING)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    return restore_exact(session, backend);
}

int vdThreadMutationCleanup(
    struct vd_thread_mutation_session* session,
    SceUID owner_pid,
    unsigned int stop_token,
    const struct vd_thread_mutation_backend* backend)
{
    if(!session || session->state == VD_THREAD_MUTATION_EMPTY)
        return 0;
    if(owner_pid != session->identity.owner_pid ||
       stop_token != session->identity.stop_token)
        return VD_KERNEL_ERROR_MUTATION_OWNER;
    if(session->state == VD_THREAD_MUTATION_SNAPSHOTTED)
    {
        clear_transaction(session, backend);
        return 0;
    }
    if(!backend || !backend->retained_target_status ||
       !session->target_retained)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    int exact_target_status = backend->retained_target_status(
        backend->context, &session->identity);
    if(exact_target_status == 0)
    {
        clear_transaction(session, backend);
        return 0;
    }
    if(exact_target_status != 1)
        return VD_KERNEL_ERROR_MUTATION_RESTORE;
    return restore_exact(session, backend);
}

int vdThreadMutationIsActive(
    const struct vd_thread_mutation_session* session)
{
    return session && session->state != VD_THREAD_MUTATION_EMPTY;
}

int vdThreadMutationGetIdentity(
    const struct vd_thread_mutation_session* session,
    struct vd_thread_mutation_identity* identity)
{
    if(!session || !identity ||
       session->state == VD_THREAD_MUTATION_EMPTY)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    copy_bytes(identity, &session->identity, sizeof(*identity));
    return 0;
}

int vdThreadMutationGetSelectedUserCoreBank(
    const struct vd_thread_mutation_session* session,
    unsigned int* register_bank)
{
    if(!session || !register_bank ||
       session->state == VD_THREAD_MUTATION_EMPTY ||
       (session->bank_mask & VD_KERNEL_THREAD_MUTATION_CORE) == 0)
        return VD_KERNEL_ERROR_MUTATION_STATE;
    const int selected = selected_user_core_bank(&session->original_core);
    if(selected < 0)
        return VD_KERNEL_ERROR_MUTATION_TARGET;
    *register_bank = (unsigned int)selected;
    return 0;
}
