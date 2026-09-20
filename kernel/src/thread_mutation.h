#pragma once

#include "vitadebug_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

enum vd_thread_mutation_state {
    VD_THREAD_MUTATION_EMPTY = 0,
    VD_THREAD_MUTATION_SNAPSHOTTED = 1,
    VD_THREAD_MUTATION_STAGED = 2,
    VD_THREAD_MUTATION_RESTORE_PENDING = 3,
};

struct vd_thread_mutation_identity {
    SceUID owner_pid;
    SceUID owner_thread;
    unsigned int stop_token;
    SceUID target_user_thread;
    SceUID target_guid;
};

struct vd_thread_mutation_backend {
    unsigned int supported_banks;
    unsigned int core_writable_register_mask;
    void* context;
    // A writable backend must retain the exact process/thread objects for the
    // full transaction so integer UID reuse cannot redirect a later write.
    // The backend owns one global transaction at a time. `retain_target` must
    // tolerate a matching `release_target` even when acquisition fails partway.
    // `release_target` must be idempotent and infallible, and must discard every
    // reference acquired by that attempted retain. `retained_target_status`
    // returns 1 only while that exact
    // retained target is alive and debug-suspended, 0 only when destruction is
    // positively proven, and a negative value when safety is unknown.
    int (*retain_target)(void* context,
                         const struct vd_thread_mutation_identity* identity);
    void (*release_target)(void* context,
                           const struct vd_thread_mutation_identity* identity);
    int (*retained_target_status)(
        void* context,
        const struct vd_thread_mutation_identity* identity);
    int (*snapshot_core)(void* context, SceUID target_guid,
                         struct vd_thread_registers* registers);
    int (*write_core)(void* context, SceUID target_guid,
                      const struct vd_thread_registers* registers);
    int (*snapshot_vfp)(void* context, SceUID target_guid,
                        struct vd_thread_vfp_registers* registers);
    int (*write_vfp)(void* context, SceUID target_guid,
                     const struct vd_thread_vfp_registers* registers);
};

struct vd_thread_mutation_session {
    uint64_t next_sequence;
    unsigned int token;
    unsigned int generation;
    unsigned int bank_mask;
    unsigned int staged_banks;
    enum vd_thread_mutation_state state;
    int target_retained;
    struct vd_thread_mutation_identity identity;
    struct vd_thread_registers original_core;
    struct vd_thread_registers desired_core;
    struct vd_thread_vfp_registers original_vfp;
    struct vd_thread_vfp_registers desired_vfp;
};

void vdThreadMutationInit(struct vd_thread_mutation_session* session);

unsigned int vdThreadMutationSupportedBanks(
    const struct vd_thread_mutation_backend* backend);

int vdThreadMutationBegin(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_begin_request* request,
    const struct vd_thread_mutation_backend* backend,
    struct vd_kernel_thread_mutation_handle* handle);

int vdThreadMutationStage(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_write_request* request,
    const struct vd_thread_mutation_backend* backend);

int vdThreadMutationCommit(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_handle* handle,
    const struct vd_thread_mutation_backend* backend);

int vdThreadMutationRestore(
    struct vd_thread_mutation_session* session,
    const struct vd_thread_mutation_identity* identity,
    const struct vd_kernel_thread_mutation_handle* handle,
    const struct vd_thread_mutation_backend* backend);

// Lease/disconnect cleanup is not driven by an untrusted handle or by a fresh
// UID lookup. The backend must classify the exact retained target as still
// suspended, definitely destroyed, or unknown.
int vdThreadMutationCleanup(
    struct vd_thread_mutation_session* session,
    SceUID owner_pid,
    unsigned int stop_token,
    const struct vd_thread_mutation_backend* backend);

int vdThreadMutationIsActive(
    const struct vd_thread_mutation_session* session);

int vdThreadMutationGetIdentity(
    const struct vd_thread_mutation_session* session,
    struct vd_thread_mutation_identity* identity);

int vdThreadMutationGetSelectedUserCoreBank(
    const struct vd_thread_mutation_session* session,
    unsigned int* register_bank);

#ifdef __cplusplus
}
#endif
