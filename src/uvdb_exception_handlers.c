#include "uvdb_exception_handlers.h"

#include <string.h>

int uvdb_exception_handlers_init(
    struct uvdb_exception_handlers* handlers,
    uvdb_exception_handler_token self)
{
    if(!handlers || !self || handlers->installed_mask ||
       handlers->ever_published_mask)
        return -1;
    memset(handlers, 0, sizeof(*handlers));
    handlers->self = self;
    return 0;
}

int uvdb_exception_handlers_restore(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context)
{
    if(!handlers)
        return -1;
    /* A never-started or already-restored registry has no kernel ownership.
     * In particular this makes shutdown-before-first-entry a true no-op. */
    if(!handlers->installed_mask)
        return 0;
    if(!backend || !backend->replace || !backend->release || !handlers->self)
        return -1;
    int result = 0;
    for(uint32_t type = UVDB_EXCEPTION_HANDLER_TYPE_COUNT; type-- > 0;)
    {
        const uint32_t bit = UINT32_C(1) << type;
        if(!(handlers->installed_mask & bit))
            continue;
        uvdb_exception_handler_token previous = handlers->previous[type];
        int restore_result;
        if(previous && previous != handlers->self)
            restore_result = backend->replace(
                context, type, previous, NULL);
        else
            restore_result = backend->release(context, type);
        if(restore_result < 0)
        {
            result = -1;
            continue;
        }
        /* Keep the captured predecessor immutable until handlers_init. A
         * callback already dispatched through our old slot can still arrive
         * after replacement and must be able to chain it during quiescence. */
        handlers->installed_mask &= ~bit;
    }
    return result;
}

int uvdb_exception_handlers_install(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context)
{
    if(!handlers || !backend || !backend->replace || !backend->release ||
       !handlers->self || handlers->installed_mask ||
       handlers->ever_published_mask)
        return -1;
    for(uint32_t type = 0; type < UVDB_EXCEPTION_HANDLER_TYPE_COUNT; ++type)
    {
        const uint32_t bit = UINT32_C(1) << type;
        /* Pass the permanent slot to the backend. KuBridge writes the old
         * handler through this pointer while holding its process spin lock and
         * only then publishes self, so a cross-core callback cannot observe
         * self before its predecessor is available here. */
        __atomic_store_n(&handlers->previous[type], 0, __ATOMIC_RELAXED);
        if(backend->replace(context, type, handlers->self,
                            &handlers->previous[type]) < 0)
        {
            (void)uvdb_exception_handlers_restore(
                handlers, backend, context);
            return -1;
        }
        uvdb_exception_handler_token previous = __atomic_load_n(
            &handlers->previous[type], __ATOMIC_ACQUIRE);
        if(previous == handlers->self)
            __atomic_store_n(&handlers->previous[type], 0,
                             __ATOMIC_RELEASE);
        handlers->ever_published_mask |= bit;
        handlers->installed_mask |= bit;
    }
    return 0;
}

int uvdb_exception_handlers_fence(
    struct uvdb_exception_handlers* handlers,
    const struct uvdb_exception_handler_backend* backend,
    void* context)
{
    if(!handlers || handlers->installed_mask ||
       !handlers->ever_published_mask || !backend || !backend->fence)
        return -1;
    if(backend->fence(context) < 0)
        return -1;
    __atomic_store_n(&handlers->fence_complete, 1u, __ATOMIC_RELEASE);
    return 0;
}

int uvdb_exception_handlers_reset_after_fence(
    struct uvdb_exception_handlers* handlers)
{
    if(!handlers || handlers->installed_mask ||
       !handlers->ever_published_mask ||
       !__atomic_load_n(&handlers->fence_complete, __ATOMIC_ACQUIRE))
        return -1;
    memset(handlers, 0, sizeof(*handlers));
    return 0;
}

uvdb_exception_handler_token uvdb_exception_handlers_previous(
    const struct uvdb_exception_handlers* handlers,
    uint32_t exception_type)
{
    if(!handlers || exception_type >= UVDB_EXCEPTION_HANDLER_TYPE_COUNT)
        return 0;
    uvdb_exception_handler_token previous = __atomic_load_n(
        &handlers->previous[exception_type], __ATOMIC_ACQUIRE);
    return previous == handlers->self ? 0 : previous;
}
