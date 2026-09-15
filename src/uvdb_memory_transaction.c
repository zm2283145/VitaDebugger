#include "uvdb_memory_transaction.h"

#include <string.h>

static int spans_overlap(const void* first, size_t first_size,
                         const void* second, size_t second_size)
{
    if(!first_size || !second_size)
        return 0;
    uintptr_t first_address = (uintptr_t)first;
    uintptr_t second_address = (uintptr_t)second;
    if(first_address <= second_address)
        return second_address - first_address < first_size;
    return first_address - second_address < second_size;
}

static int verify_bytes(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const unsigned char* expected,
    size_t size)
{
    unsigned char observed[64];
    size_t offset = 0;
    while(offset < size)
    {
        size_t chunk = size - offset;
        if(chunk > sizeof(observed))
            chunk = sizeof(observed);
        if(io->read(context, address + offset, observed, chunk) != chunk ||
           memcmp(observed, expected + offset, chunk) != 0)
            return -1;
        offset += chunk;
    }
    return 0;
}

static int restore_original(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const unsigned char* original,
    size_t size)
{
    if(io->write(context, address, original, size) != size ||
       (io->sync && io->sync(context, address, size) < 0) ||
       verify_bytes(io, context, address, original, size) < 0)
        return UVDB_MEMORY_TRANSACTION_RESTORE_PENDING;
    return UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED;
}

int uvdb_memory_write_transaction(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const void* input,
    size_t size,
    void* scratch,
    size_t scratch_size)
{
    if(!io || !io->read || !io->write ||
       (size && (!input || !scratch)) || scratch_size < size ||
       (size && address > UINTPTR_MAX - (size - 1u)) ||
       spans_overlap(input, size, scratch, size))
        return UVDB_MEMORY_TRANSACTION_INVALID;
    if(!size)
        return UVDB_MEMORY_TRANSACTION_OK;

    unsigned char* original = scratch;
    if(io->read(context, address, original, size) != size)
        return UVDB_MEMORY_TRANSACTION_READ_FAILED;

    if(io->write(context, address, input, size) != size ||
       (io->sync && io->sync(context, address, size) < 0) ||
       verify_bytes(io, context, address, input, size) < 0)
        return restore_original(io, context, address, original, size);
    return UVDB_MEMORY_TRANSACTION_OK;
}
