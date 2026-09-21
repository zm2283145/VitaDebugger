#pragma once

#include <stdint.h>

#define UVDB_EXCEPTION_HANDLER_TYPE_COUNT 3u

typedef uintptr_t uvdb_exception_handler_token;

struct uvdb_exception_handler_backend {
    /* When `previous` is non-NULL, replace must durably store the displaced
     * token there before `replacement` can be observed by a dispatcher. A
     * negative return must mean replacement was never published. This is the
     * publication ordering supplied by KuBridge while it holds the process
     * exception-handler spin lock. */
    int (*replace)(void* context, uint32_t exception_type,
                   uvdb_exception_handler_token replacement,
                   uvdb_exception_handler_token* previous);
    int (*release)(void* context, uint32_t exception_type);
    /* Optional ABI fence. Success must guarantee that no callback which
     * observed any pre-fence slot value can enter after this call returns.
     * Slot replacement and a user-space active count are not such a fence. */
    int (*fence)(void* context);
};

struct uvdb_exception_handlers {
    uvdb_exception_handler_token self;
    uvdb_exception_handler_token previous[
        UVDB_EXCEPTION_HANDLER_TYPE_COUNT];
    uint32_t installed_mask;
    /* Monotonic for this registry generation. Once any callback has been
     * published, captured predecessor tokens are process-lifetime state: the
     * current KuBridge ABI has no dispatcher fence with which to prove that a
     * copied callback cannot arrive late. */
    uint32_t ever_published_mask;
    uint32_t fence_complete;
};

/* `handlers` must initially be zero-initialized. Reinitialization is rejected
 * after any successful publication so late callbacks cannot observe erased or
 * next-generation predecessor tokens. */
int uvdb_exception_handlers_init(
    struct uvdb_exception_handlers* handlers,
    uvdb_exception_handler_token self);

/* Install all three handler types. On a partial failure, already-installed
 * types are restored in reverse order. A failed rollback remains represented
 * in installed_mask so teardown can be retried instead of silently unloading
 * with uncertain ownership. Any successfully published slot remains recorded
 * in ever_published_mask even after rollback. */
int uvdb_exception_handlers_install(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context);

/* Restore each exact predecessor (or release a NULL predecessor) in reverse
 * order. Failed slots retain their installed bit. Captured predecessor tokens
 * remain immutable after successful restore for this terminal registry
 * generation, covering callbacks dispatched before restoration but entering
 * later. The current KuBridge ABI has no fence that permits safe reuse. */
int uvdb_exception_handlers_restore(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context);

/* Fence first, then let the caller drain callbacks which may have entered
 * during the fence. Missing or failed support retains the generation. */
int uvdb_exception_handlers_fence(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context);

/* Call only after a successful fence and a subsequent user-side active
 * callback drain. */
int uvdb_exception_handlers_reset_after_fence(
    struct uvdb_exception_handlers* handlers);

uvdb_exception_handler_token uvdb_exception_handlers_previous(
    const struct uvdb_exception_handlers* handlers,
    uint32_t exception_type);
