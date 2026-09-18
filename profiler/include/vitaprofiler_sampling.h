#ifndef VITAPROFILER_SAMPLING_H
#define VITAPROFILER_SAMPLING_H

#include "vitaprofiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VP_SAMPLE_PROVIDER_ABI_VERSION 1u
#define VP_SAMPLE_MAX_FRAMES 64u

#define VP_SAMPLE_CAP_CURRENT_THREAD_PC (UINT32_C(1) << 0)
#define VP_SAMPLE_CAP_CURRENT_THREAD_STACK (UINT32_C(1) << 1)
#define VP_SAMPLE_CAP_FOREIGN_THREAD_PC (UINT32_C(1) << 2)
#define VP_SAMPLE_CAP_FOREIGN_THREAD_STACK (UINT32_C(1) << 3)
#define VP_SAMPLE_CAP_STABLE_IDENTITY (UINT32_C(1) << 4)
#define VP_SAMPLE_CAP_BOUNDED_STACK_READ (UINT32_C(1) << 5)
#define VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE (UINT32_C(1) << 6)
#define VP_SAMPLE_CAP_ALL                                                   \
    (VP_SAMPLE_CAP_CURRENT_THREAD_PC | VP_SAMPLE_CAP_CURRENT_THREAD_STACK | \
     VP_SAMPLE_CAP_FOREIGN_THREAD_PC |                                     \
     VP_SAMPLE_CAP_FOREIGN_THREAD_STACK |                                  \
     VP_SAMPLE_CAP_STABLE_IDENTITY | VP_SAMPLE_CAP_BOUNDED_STACK_READ |     \
     VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE)

#define VP_SAMPLE_FRAME_FLAG_THUMB (UINT32_C(1) << 0)
#define VP_SAMPLE_FRAME_FLAG_CONTEXT_CONFIDENT (UINT32_C(1) << 1)
#define VP_SAMPLE_FRAME_FLAG_ALL                                           \
    (VP_SAMPLE_FRAME_FLAG_THUMB | VP_SAMPLE_FRAME_FLAG_CONTEXT_CONFIDENT)

enum vp_sample_target_kind {
    VP_SAMPLE_TARGET_CURRENT = 1,
    VP_SAMPLE_TARGET_FOREIGN = 2,
};

enum vp_sample_stop_reason {
    VP_SAMPLE_STOP_NONE = 0,
    VP_SAMPLE_STOP_PC_ONLY = 1,
    VP_SAMPLE_STOP_COMPLETE = 2,
    VP_SAMPLE_STOP_DEPTH_LIMIT = 3,
    VP_SAMPLE_STOP_PROVIDER_ERROR = 4,
    VP_SAMPLE_STOP_STALE_IDENTITY = 5,
    VP_SAMPLE_STOP_MALFORMED = 6,
    VP_SAMPLE_STOP_RELEASE_ERROR = 7,
};

struct vp_sample_target {
    uint32_t kind;
    int32_t thread_id;
    uint64_t identity;
};

struct vp_sample_cursor {
    uint32_t pc;
    uint32_t sp;
    uint32_t frame_pointer;
    uint32_t flags;
    uint64_t provider_state[2];
};

struct vp_sample_capture {
    struct vp_sample_cursor cursor;
    uint64_t identity;
    int32_t thread_id;
    uint32_t reserved;
};

struct vp_sample_frame {
    uint32_t pc;
    uint32_t sp;
    uint32_t flags;
};

/*
 * begin_sample must acquire a nonzero lease token and retain every identity or
 * memory lifetime needed by next_frame. A failed begin must return no token.
 * For foreign targets, capture must repeat the requested thread_id and exact
 * identity; a mismatch is treated as stale even if begin reported success.
 * That identity must include a logical lifetime epoch and become stale after
 * thread exit even if a retained platform object remains restartable.
 * Foreign capture must also set CONTEXT_CONFIDENT only after resolving any
 * architecture-specific register-bank ambiguity without relying on historical
 * bank names.
 * PC-only providers may leave cursor.sp and cursor.frame_pointer zero.
 *
 * next_frame performs provider-specific unwinding. The portable core never
 * assumes an ARM frame layout or dereferences target memory. Return END only
 * for a clean end of stack. A read/unwind error after the initial PC produces
 * a bounded PARTIAL sample. A provider advertising a stack capability must
 * also advertise BOUNDED_STACK_READ and reject every read outside bounds it
 * has independently validated for that target.
 *
 * end_sample releases the lease without changing target state. It must be
 * retryable after failure. The provider function table must not change while
 * active. Its writable user data must outlive the sampler, including any
 * pending release retry, and is accessed only under the provider's own
 * synchronization policy.
 */
typedef int (*vp_sample_begin_fn)(
    void* user, const struct vp_sample_target* target, uint32_t max_depth,
    uint64_t* lease_token, struct vp_sample_capture* capture);
typedef int (*vp_sample_next_frame_fn)(
    void* user, uint64_t lease_token,
    const struct vp_sample_cursor* current,
    struct vp_sample_cursor* next);
typedef int (*vp_sample_end_fn)(void* user, uint64_t lease_token);

struct vp_sample_provider {
    uint32_t abi_version;
    uint32_t capabilities;
    vp_sample_begin_fn begin_sample;
    vp_sample_next_frame_fn next_frame;
    vp_sample_end_fn end_sample;
    void* user;
};

struct vp_sampler_config {
    struct vp_sample_frame* frames;
    uint32_t frame_capacity;
    uint32_t max_depth;
    uint32_t required_capabilities;
    uint32_t reserved;
};

/* Caller-owned storage. Zero-initialize before init and treat as private.
 * Calls are serialized: one sampler cannot be sampled concurrently. */
struct vp_sampler {
    const struct vp_sample_provider* provider;
    struct vp_sample_frame* frames;
    uint64_t active_token;
    int32_t last_provider_error;
    uint32_t frame_capacity;
    uint32_t max_depth;
    uint32_t capabilities;
    uint32_t active;
    uint32_t release_pending;
    uint32_t initialized;
};

struct vp_sample {
    /* Borrowed view of vp_sampler_config.frames. A later sampling attempt,
     * including one that fails, clears and overwrites this storage. */
    struct vp_sample_frame* frames;
    uint64_t identity;
    int32_t thread_id;
    uint32_t frame_capacity;
    uint32_t frame_count;
    uint32_t stop_reason;
    int32_t provider_error;
    uint32_t reserved;
};

struct vp_sampler_status {
    uint32_t capabilities;
    uint32_t max_depth;
    uint32_t active;
    uint32_t release_pending;
    int32_t last_provider_error;
};

/*
 * required_capabilities is an explicit opt-in and must be a subset of the
 * provider's advertised capabilities. Stack capabilities require their PC
 * counterpart and BOUNDED_STACK_READ. Foreign capabilities additionally
 * require STABLE_IDENTITY and FOREIGN_CONTEXT_CONFIDENCE.
 */
int vp_sampler_init(struct vp_sampler* sampler,
                    const struct vp_sample_provider* provider,
                    const struct vp_sampler_config* config);
int vp_sampler_deinit(struct vp_sampler* sampler);

/* Current-thread callbacks execute synchronously on the calling thread.
 * Foreign sampling requires a positive thread ID and nonzero stable identity.
 * Neither function starts, stops, suspends, or resumes a thread. */
int vp_sampler_sample_current(struct vp_sampler* sampler,
                              struct vp_sample* sample);
int vp_sampler_sample_foreign(struct vp_sampler* sampler,
                              int32_t thread_id, uint64_t identity,
                              struct vp_sample* sample);

/* A failed end_sample quarantines the sampler. No new sample is admitted until
 * this retry succeeds; deinit also refuses to discard the obligation. */
int vp_sampler_retry_release(struct vp_sampler* sampler);
int vp_sampler_get_status(const struct vp_sampler* sampler,
                          struct vp_sampler_status* status);

#ifdef __cplusplus
}
#endif

#endif
