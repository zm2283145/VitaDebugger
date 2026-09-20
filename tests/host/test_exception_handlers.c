#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_exception_handlers.h"

struct fake_kernel {
    uvdb_exception_handler_token slots[UVDB_EXCEPTION_HANDLER_TYPE_COUNT];
    struct uvdb_exception_handlers* dispatch_handlers;
    uvdb_exception_handler_token dispatch_previous;
    int replace_calls;
    int release_calls;
    int fail_replace_call;
    int fail_release_call;
    int dispatch_replace_call;
    int dispatch_observed;
    int fence_calls;
    int fail_fence;
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

static int fake_replace(
    void* opaque,
    uint32_t type,
    uvdb_exception_handler_token replacement,
    uvdb_exception_handler_token* previous)
{
    struct fake_kernel* kernel = opaque;
    kernel->replace_calls++;
    if(type >= UVDB_EXCEPTION_HANDLER_TYPE_COUNT ||
       kernel->replace_calls == kernel->fail_replace_call)
        return -1;
    if(previous)
        *previous = kernel->slots[type];
    kernel->slots[type] = replacement;
    /* Model KuBridge's critical ordering: the old callback is copied to the
     * supplied user slot, the replacement becomes visible, and only then does
     * the registration call return. This is the callback window that used to
     * observe an unpublished predecessor in the registry. */
    if(kernel->dispatch_handlers &&
       kernel->replace_calls == kernel->dispatch_replace_call)
    {
        kernel->dispatch_observed = 1;
        kernel->dispatch_previous = uvdb_exception_handlers_previous(
            kernel->dispatch_handlers, type);
    }
    return 0;
}

static int fake_release(void* opaque, uint32_t type)
{
    struct fake_kernel* kernel = opaque;
    kernel->release_calls++;
    if(type >= UVDB_EXCEPTION_HANDLER_TYPE_COUNT ||
       kernel->release_calls == kernel->fail_release_call)
        return -1;
    kernel->slots[type] = 0;
    return 0;
}

static const struct uvdb_exception_handler_backend backend = {
    .replace = fake_replace,
    .release = fake_release,
};

static int fake_fence(void* opaque)
{
    struct fake_kernel* kernel = opaque;
    kernel->fence_calls++;
    return kernel->fail_fence ? -1 : 0;
}

static const struct uvdb_exception_handler_backend fenced_backend = {
    .replace = fake_replace,
    .release = fake_release,
    .fence = fake_fence,
};

static void seed(struct fake_kernel* kernel)
{
    memset(kernel, 0, sizeof(*kernel));
    kernel->slots[0] = 0x1001;
    kernel->slots[1] = 0;
    kernel->slots[2] = 0x3001;
}

int main(void)
{
    const uvdb_exception_handler_token self = 0xd00d;
    struct fake_kernel kernel;
    struct uvdb_exception_handlers handlers = {0};

    /* Shutdown can race server startup before the first uvdb_enter. With no
     * published handler, restoration is a true no-op and does not require a
     * populated self token. */
    memset(&kernel, 0, sizeof(kernel));
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) == 0 &&
          kernel.replace_calls == 0 && kernel.release_calls == 0,
          "zero-initialized handler restoration is a no-op");

    seed(&kernel);
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "initialize first handler registry generation");
    kernel.dispatch_handlers = &handlers;
    kernel.dispatch_replace_call = 1;
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) == 0 &&
          handlers.installed_mask == 7 &&
          handlers.ever_published_mask == 7 &&
          kernel.slots[0] == self && kernel.slots[1] == self &&
          kernel.slots[2] == self,
          "install captures all fake-kernel predecessor slots");
    check(kernel.dispatch_observed &&
              kernel.dispatch_previous == 0x1001,
          "published callback sees predecessor before replace returns");
    check(uvdb_exception_handlers_previous(&handlers, 0) == 0x1001 &&
          uvdb_exception_handlers_previous(&handlers, 1) == 0 &&
          uvdb_exception_handlers_previous(&handlers, 2) == 0x3001,
          "previous lookup preserves each distinct handler");
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) == 0 &&
          handlers.installed_mask == 0 &&
          kernel.slots[0] == 0x1001 && kernel.slots[1] == 0 &&
          kernel.slots[2] == 0x3001 &&
          uvdb_exception_handlers_previous(&handlers, 0) == 0x1001 &&
          uvdb_exception_handlers_previous(&handlers, 2) == 0x3001,
          "teardown restores exact predecessors and NULL default");
    check(handlers.previous[0] == 0x1001 &&
              handlers.previous[2] == 0x3001,
           "restored predecessor tokens remain stable for late dispatch");

    seed(&kernel);
    memset(&handlers, 0, sizeof(handlers));
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "initialize partial-install registry generation");
    kernel.fail_replace_call = 3;
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) < 0 &&
          handlers.installed_mask == 0 &&
          handlers.ever_published_mask == 3 &&
          kernel.slots[0] == 0x1001 && kernel.slots[1] == 0 &&
          kernel.slots[2] == 0x3001,
          "partial install failure rolls back earlier fake slots");
    const struct uvdb_exception_handlers rolled_back_handlers = handlers;
    const int rolled_back_replace_calls = kernel.replace_calls;
    check(uvdb_exception_handlers_init(&handlers, self) < 0 &&
              memcmp(&handlers, &rolled_back_handlers,
                     sizeof(handlers)) == 0,
          "ever-published generation cannot erase late-callback tokens");
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) < 0 &&
              kernel.replace_calls == rolled_back_replace_calls &&
              memcmp(&handlers, &rolled_back_handlers,
                     sizeof(handlers)) == 0,
          "rolled-back generation cannot publish replacement handlers again");

    seed(&kernel);
    memset(&handlers, 0, sizeof(handlers));
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "initialize rollback-failure registry generation");
    kernel.fail_replace_call = 3;
    kernel.fail_release_call = 1;
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) < 0 &&
          handlers.installed_mask == (1u << 1) &&
          handlers.previous[1] == 0 && kernel.slots[1] == self,
          "failed partial-install rollback retains NULL restore obligation");
    kernel.fail_replace_call = 0;
    kernel.fail_release_call = 0;
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) == 0 &&
          handlers.installed_mask == 0 && kernel.slots[1] == 0,
          "retained NULL restore obligation succeeds on retry");

    seed(&kernel);
    memset(&handlers, 0, sizeof(handlers));
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "initialize restore-retry registry generation");
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) == 0,
          "install restoration-failure fixture");
    kernel.fail_replace_call = kernel.replace_calls + 1;
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) < 0 &&
          (handlers.installed_mask & (1u << 2)) != 0 &&
          handlers.previous[2] == 0x3001,
          "failed predecessor restore retains exact retry obligation");
    kernel.fail_replace_call = 0;
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) == 0 &&
          handlers.installed_mask == 0 && kernel.slots[2] == 0x3001,
          "retained restore obligation succeeds on retry");

    seed(&kernel);
    kernel.slots[0] = self;
    memset(&handlers, 0, sizeof(handlers));
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "initialize self-predecessor registry generation");
    check(uvdb_exception_handlers_install(
              &handlers, &backend, &kernel) == 0 &&
          uvdb_exception_handlers_previous(&handlers, 0) == 0,
          "self predecessor is never exposed for recursive chaining");
    check(uvdb_exception_handlers_restore(
              &handlers, &backend, &kernel) == 0 &&
          kernel.slots[0] == 0,
          "orphan self predecessor releases to default on teardown");

    check(uvdb_exception_handlers_fence(
              &handlers, &backend, &kernel) < 0 &&
          handlers.ever_published_mask != 0,
          "missing callback fence fails closed without erasing generation");
    kernel.fail_fence = 1;
    check(uvdb_exception_handlers_fence(
              &handlers, &fenced_backend, &kernel) < 0 &&
          handlers.ever_published_mask != 0 && kernel.fence_calls == 1,
          "failed callback fence retains predecessor lifetime state");
    kernel.fail_fence = 0;
    check(uvdb_exception_handlers_fence(
              &handlers, &fenced_backend, &kernel) == 0 &&
          handlers.ever_published_mask != 0 &&
          handlers.fence_complete == 1 && kernel.fence_calls == 2,
          "successful kernel fence retains state for callback drain");
    check(uvdb_exception_handlers_reset_after_fence(
              &handlers) == 0 &&
          handlers.ever_published_mask == 0 &&
          handlers.self == 0,
          "post-fence callback drain authorizes registry reset");
    check(uvdb_exception_handlers_init(&handlers, self) == 0,
          "new handler generation is allowed only after successful fence");

    if(failures)
        return 1;
    puts("PASS: fake-kernel exception-handler install/restore lifecycle");
    return 0;
}
