#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_memory_transaction.h"

struct fake_kernel {
    unsigned char memory[256];
    size_t read_limit;
    size_t write_limit;
    size_t first_write_limit;
    int read_calls;
    int write_calls;
    int fail_sync_call;
    int sync_calls;
    int corrupt_after_write;
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

static size_t fake_read(void* opaque, uintptr_t address, void* output,
                        size_t size)
{
    struct fake_kernel* kernel = opaque;
    kernel->read_calls++;
    if(address > sizeof(kernel->memory) ||
       size > sizeof(kernel->memory) - address)
        return 0;
    size_t count = size;
    if(kernel->read_limit < count)
        count = kernel->read_limit;
    memcpy(output, kernel->memory + address, count);
    return count;
}

static size_t fake_write(void* opaque, uintptr_t address, const void* input,
                         size_t size)
{
    struct fake_kernel* kernel = opaque;
    kernel->write_calls++;
    if(address > sizeof(kernel->memory) ||
       size > sizeof(kernel->memory) - address)
        return 0;
    size_t count = size;
    if(kernel->write_limit < count)
        count = kernel->write_limit;
    if(kernel->write_calls == 1 && kernel->first_write_limit < count)
        count = kernel->first_write_limit;
    memcpy(kernel->memory + address, input, count);
    if(kernel->corrupt_after_write && count)
    {
        kernel->memory[address + count - 1u] ^= 0x80u;
        kernel->corrupt_after_write = 0;
    }
    return count;
}

static int fake_sync(void* opaque, uintptr_t address, size_t size)
{
    (void)address;
    (void)size;
    struct fake_kernel* kernel = opaque;
    kernel->sync_calls++;
    return kernel->fail_sync_call == kernel->sync_calls ? -1 : 0;
}

static const struct uvdb_memory_transaction_io fake_io = {
    .read = fake_read,
    .write = fake_write,
    .sync = fake_sync,
};

static void reset_kernel(struct fake_kernel* kernel)
{
    memset(kernel, 0, sizeof(*kernel));
    for(size_t i = 0; i < sizeof(kernel->memory); ++i)
        kernel->memory[i] = (unsigned char)i;
    kernel->read_limit = SIZE_MAX;
    kernel->write_limit = SIZE_MAX;
    kernel->first_write_limit = SIZE_MAX;
}

int main(void)
{
    struct fake_kernel kernel;
    unsigned char input[32];
    unsigned char scratch[32];
    memset(input, 0xa5, sizeof(input));

    reset_kernel(&kernel);
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, 64, input, sizeof(input), scratch,
              sizeof(scratch)) == UVDB_MEMORY_TRANSACTION_OK &&
          memcmp(kernel.memory + 64, input, sizeof(input)) == 0,
          "fake kernel successful transaction commits exact bytes");

    reset_kernel(&kernel);
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, UINTPTR_MAX, NULL, 0, NULL, 0) ==
              UVDB_MEMORY_TRANSACTION_OK &&
          kernel.read_calls == 0 && kernel.write_calls == 0,
          "zero-length transaction performs no fake-kernel operation");

    reset_kernel(&kernel);
    unsigned char original[32];
    memcpy(original, kernel.memory + 64, sizeof(original));
    kernel.read_limit = 31;
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, 64, input, sizeof(input), scratch,
              sizeof(scratch)) == UVDB_MEMORY_TRANSACTION_READ_FAILED &&
          memcmp(kernel.memory + 64, original, sizeof(original)) == 0,
          "short preflight read performs no mutation");

    reset_kernel(&kernel);
    memcpy(original, kernel.memory + 64, sizeof(original));
    kernel.write_limit = 8;
    /* First write is short. Let the rollback use its full extent. */
    int result = uvdb_memory_write_transaction(
        &fake_io, &kernel, 64, input, sizeof(input), scratch,
        sizeof(scratch));
    check(result == UVDB_MEMORY_TRANSACTION_RESTORE_PENDING,
          "persistent short writes retain restoration obligation");

    reset_kernel(&kernel);
    memcpy(original, kernel.memory + 64, sizeof(original));
    kernel.first_write_limit = 8;
    result = uvdb_memory_write_transaction(
        &fake_io, &kernel, 64, input, sizeof(input), scratch,
        sizeof(scratch));
    check(result == UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED,
          "transient short write reports a verified rollback");
    check(kernel.write_calls == 2,
          "transient short write performs exactly one rollback write");
    check(kernel.read_calls == 2,
          "transient short write snapshots and verifies the rollback");
    check(memcmp(kernel.memory + 64, original, sizeof(original)) == 0,
          "transient short write restores exact original bytes");

    reset_kernel(&kernel);
    memcpy(original, kernel.memory + 64, sizeof(original));
    kernel.fail_sync_call = 1;
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, 64, input, sizeof(input), scratch,
              sizeof(scratch)) ==
              UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED &&
          memcmp(kernel.memory + 64, original, sizeof(original)) == 0,
          "sync failure rolls back and verifies original bytes");

    reset_kernel(&kernel);
    memcpy(original, kernel.memory + 64, sizeof(original));
    kernel.corrupt_after_write = 1;
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, 64, input, sizeof(input), scratch,
              sizeof(scratch)) ==
              UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED &&
          memcmp(kernel.memory + 64, original, sizeof(original)) == 0,
          "readback mismatch rolls back exact original bytes");

    reset_kernel(&kernel);
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, UINTPTR_MAX - 3u, input, 8u, scratch,
              sizeof(scratch)) == UVDB_MEMORY_TRANSACTION_INVALID,
          "wrapped address range rejected before fake kernel call");
    unsigned char final_byte = 0x5a;
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, sizeof(kernel.memory) - 1u,
              &final_byte, 1u, scratch, sizeof(scratch)) ==
              UVDB_MEMORY_TRANSACTION_OK &&
          kernel.memory[sizeof(kernel.memory) - 1u] == final_byte,
          "one byte at the final mapped address remains valid");
    check(uvdb_memory_write_transaction(
              &fake_io, &kernel, 0, scratch, 8u, scratch,
              sizeof(scratch)) == UVDB_MEMORY_TRANSACTION_INVALID,
          "aliased input and rollback scratch rejected");

    if(failures)
        return 1;
    puts("PASS: fake-kernel memory mutation rollback");
    return 0;
}
