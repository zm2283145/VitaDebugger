#pragma once

#include <stdint.h>

#include "thread_mutation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_THREAD_SETTER_BINDING_VERSION 1u
#define VD_THREAD_SETTER_FINGERPRINT_SIZE 32u

enum vd_thread_reference_release_result {
    VD_THREAD_REFERENCE_RELEASED = 0,
    /* The adapter proves the reference is still owned and may be retried. */
    VD_THREAD_REFERENCE_RELEASE_RETAINED = 1,
};

/*
 * A platform adapter may return a binding only after it has resolved a fixed
 * allowlist of symbols and matched the complete module/code identity expected
 * by that adapter.  This generic layer deliberately does not know, infer, or
 * cast an undocumented firmware prototype.
 */
struct vd_thread_setter_binding {
    unsigned int struct_size;
    unsigned int version;
    unsigned int supported_banks;
    SceUID module_id;
    unsigned int module_nid;
    uintptr_t core_get;
    uintptr_t core_set;
    uintptr_t vfp_get;
    uintptr_t vfp_set;
    unsigned char fingerprint[VD_THREAD_SETTER_FINGERPRINT_SIZE];
    unsigned int reserved[4];
};

/*
 * `object` is an opaque, retained kernel-object identity.  Setter adapters
 * receive this exact reference as well as the original GUID.  The adapter is
 * required to use the retained object, never a fresh lookup of the GUID.
 */
struct vd_thread_target_reference {
    SceUID guid;
    uintptr_t object;
    uint64_t generation;
};

struct vd_thread_mutation_provider_ops {
    void* context;

    /*
     * Resolve and authenticate one immutable setter binding.  Success means
     * the adapter has matched a fixed module identity and code fingerprint,
     * not merely that a NID returned a non-null address.
     */
    int (*resolve_verified_binding)(
        void* context,
        struct vd_thread_setter_binding* binding);

    /*
     * Return 1 only while the exact resolved module/code identity is still
     * loaded, 0 when replacement/unload is positively known, and <0 when the
     * result is uncertain.
     */
    int (*binding_status)(
        void* context,
        const struct vd_thread_setter_binding* binding);

    int (*retain_process)(
        void* context,
        SceUID owner_pid,
        struct vd_thread_target_reference* reference);
    int (*retain_thread)(
        void* context,
        SceUID target_guid,
        struct vd_thread_target_reference* reference);

    /*
     * Each retain callback is atomic at this boundary: a negative return must
     * leave no reference behind, while success must fill a valid, owned
     * reference.  The provider handles a successful process retain followed
     * by a failed thread retain and releases the process during unwind.
     */
    /*
     * Return VD_THREAD_REFERENCE_RELEASED only after definite release,
     * VD_THREAD_REFERENCE_RELEASE_RETAINED only when definitely unchanged and
     * safe to retry, or a negative value when the effect is unknown.
     */
    int (*release_reference)(
        void* context,
        const struct vd_thread_target_reference* reference);

    /*
     * Acquire adapter-owned serialization and revalidate/pin the binding,
     * process, thread, parent relation, and debug-suspended state as one access
     * gate.  A return of 1 holds that protection through `end_target_access`;
     * 0 means definitively unavailable and <0 means uncertain.  End is
     * infallible and must not release either retained object reference.
     */
    int (*begin_target_access)(
        void* context,
        const struct vd_thread_setter_binding* binding,
        const struct vd_thread_target_reference* process,
        const struct vd_thread_target_reference* thread,
        const struct vd_thread_mutation_identity* identity);
    void (*end_target_access)(void* context);

    /*
     * Return 1 only while the exact retained process/thread are alive, still
     * related to `identity`, and the thread is debug-suspended.  Return 0 only
     * after definite process/thread destruction.  UID mismatch, inventory
     * failure, or an indeterminate suspend state must return <0.
     */
    int (*retained_target_status)(
        void* context,
        const struct vd_thread_target_reference* process,
        const struct vd_thread_target_reference* thread,
        const struct vd_thread_mutation_identity* identity);

    int (*snapshot_core)(
        void* context,
        const struct vd_thread_setter_binding* binding,
        const struct vd_thread_target_reference* thread,
        struct vd_thread_registers* registers);
    int (*write_core)(
        void* context,
        const struct vd_thread_setter_binding* binding,
        const struct vd_thread_target_reference* thread,
        const struct vd_thread_registers* registers);
    int (*snapshot_vfp)(
        void* context,
        const struct vd_thread_setter_binding* binding,
        const struct vd_thread_target_reference* thread,
        struct vd_thread_vfp_registers* registers);
    int (*write_vfp)(
        void* context,
        const struct vd_thread_setter_binding* binding,
        const struct vd_thread_target_reference* thread,
        const struct vd_thread_vfp_registers* registers);
};

struct vd_thread_mutation_provider {
    struct vd_thread_mutation_backend backend;
    struct vd_thread_mutation_provider_ops ops;
    struct vd_thread_setter_binding binding;
    struct vd_thread_mutation_identity identity;
    struct vd_thread_target_reference process;
    struct vd_thread_target_reference thread;
    unsigned int configured_banks;
    int binding_ready;
    int lease_active;
    int process_retained;
    int thread_retained;
    int release_pending;
    int release_retryable;
    int release_uncertain;
    int acquisition_uncertain;
    int shutdown_requested;
    int last_release_error;
};

/*
 * Initialize zero-filled storage once and attempt one verified bind. A missing,
 * malformed, or unverifiable binding is not fatal: the returned backend simply
 * advertises zero writable banks. Reinitializing any configured, active, or
 * quarantined provider is rejected without changing it.
 */
int vdThreadMutationProviderInit(
    struct vd_thread_mutation_provider* provider,
    const struct vd_thread_mutation_provider_ops* ops);

const struct vd_thread_mutation_backend* vdThreadMutationProviderBackend(
    struct vd_thread_mutation_provider* provider);

/*
 * Retry only a release for which the adapter explicitly returned
 * VD_THREAD_REFERENCE_RELEASE_RETAINED. A negative release result has unknown
 * effect and is quarantined without retry. No new transaction is admitted
 * until this returns zero. An identity-contract violation remains a fatal,
 * non-drainable quarantine.
 */
int vdThreadMutationProviderDrain(
    struct vd_thread_mutation_provider* provider);

/*
 * Permanently disable new transactions before the adapter/plugin unloads.
 * The caller must serialize this with all provider entry points. A busy or
 * cleanup error means the adapter must remain loaded so its callbacks and
 * retained references stay valid. Retryable releases may still be drained
 * after shutdown is requested; ambiguous acquisition/release effects are a
 * non-drainable quarantine.
 */
int vdThreadMutationProviderPrepareUnload(
    struct vd_thread_mutation_provider* provider);

int vdThreadMutationProviderReady(
    struct vd_thread_mutation_provider* provider);

int vdThreadMutationProviderLastReleaseError(
    const struct vd_thread_mutation_provider* provider);

#ifdef __cplusplus
}
#endif
