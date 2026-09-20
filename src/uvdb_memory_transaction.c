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

static void clear_transaction(
    struct uvdb_memory_transaction* transaction)
{
    transaction->address = 0;
    transaction->size = 0;
    transaction->state = UVDB_MEMORY_TRANSACTION_IDLE;
}

static int restore_original(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const unsigned char* original,
    size_t size)
{
    size_t written = io->write(context, address, original, size);
    int sync_result = io->sync
        ? io->sync(context, address, size)
        : 0;
    int verify_result = verify_bytes(
        io, context, address, original, size);
    return written == size && sync_result >= 0 && verify_result == 0
        ? UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED
        : UVDB_MEMORY_TRANSACTION_RESTORE_PENDING;
}

void uvdb_memory_transaction_init(
    struct uvdb_memory_transaction* transaction,
    void* storage,
    size_t capacity)
{
    if(!transaction)
        return;
    memset(transaction, 0, sizeof(*transaction));
    transaction->original = storage;
    transaction->capacity = capacity;
}

int uvdb_memory_transaction_prepare(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    size_t size)
{
    if(!transaction || !io || !io->read ||
       transaction->state != UVDB_MEMORY_TRANSACTION_IDLE ||
       (size && !transaction->original) || size > transaction->capacity ||
       (size && address > UINTPTR_MAX - (size - 1u)))
        return UVDB_MEMORY_TRANSACTION_INVALID;
    if(!size)
        return UVDB_MEMORY_TRANSACTION_OK;
    if(io->read(context, address, transaction->original, size) != size)
        return UVDB_MEMORY_TRANSACTION_READ_FAILED;

    transaction->address = address;
    transaction->size = size;
    transaction->state = UVDB_MEMORY_TRANSACTION_PREPARED;
    return UVDB_MEMORY_TRANSACTION_OK;
}

int uvdb_memory_transaction_restore(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context)
{
    if(!transaction || !io || !io->read || !io->write ||
       (transaction->state != UVDB_MEMORY_TRANSACTION_PREPARED &&
        transaction->state != UVDB_MEMORY_TRANSACTION_APPLIED) ||
       !transaction->original || !transaction->size ||
       transaction->size > transaction->capacity)
        return UVDB_MEMORY_TRANSACTION_STATE_ERROR;
    int result = restore_original(
        io, context, transaction->address, transaction->original,
        transaction->size);
    if(result != UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED)
        return result;
    clear_transaction(transaction);
    return result;
}

int uvdb_memory_transaction_apply(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context,
    const void* input,
    size_t size)
{
    if(!transaction || !io || !io->read || !io->write || !input ||
       transaction->state != UVDB_MEMORY_TRANSACTION_PREPARED ||
       size != transaction->size ||
       spans_overlap(input, size, transaction->original, transaction->size))
        return UVDB_MEMORY_TRANSACTION_INVALID;
    if(io->write(context, transaction->address, input, size) != size ||
       (io->sync && io->sync(context, transaction->address, size) < 0) ||
       verify_bytes(io, context, transaction->address, input, size) < 0)
        return uvdb_memory_transaction_restore(
            transaction, io, context);
    transaction->state = UVDB_MEMORY_TRANSACTION_APPLIED;
    return UVDB_MEMORY_TRANSACTION_OK;
}

int uvdb_memory_transaction_commit(
    struct uvdb_memory_transaction* transaction)
{
    if(!transaction ||
       transaction->state != UVDB_MEMORY_TRANSACTION_APPLIED)
        return UVDB_MEMORY_TRANSACTION_STATE_ERROR;
    clear_transaction(transaction);
    return UVDB_MEMORY_TRANSACTION_OK;
}

int uvdb_memory_transaction_is_pending(
    const struct uvdb_memory_transaction* transaction)
{
    return transaction &&
        transaction->state != UVDB_MEMORY_TRANSACTION_IDLE;
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

    struct uvdb_memory_transaction transaction;
    uvdb_memory_transaction_init(&transaction, scratch, scratch_size);
    int result = uvdb_memory_transaction_prepare(
        &transaction, io, context, address, size);
    if(result != UVDB_MEMORY_TRANSACTION_OK)
        return result;
    result = uvdb_memory_transaction_apply(
        &transaction, io, context, input, size);
    if(result != UVDB_MEMORY_TRANSACTION_OK)
        return result;
    return uvdb_memory_transaction_commit(&transaction);
}

int uvdb_memory_restore_transaction(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    size_t size,
    const void* scratch,
    size_t scratch_size)
{
    if(!io || !io->read || !io->write ||
       (size && !scratch) || scratch_size < size ||
       (size && address > UINTPTR_MAX - (size - 1u)))
        return UVDB_MEMORY_TRANSACTION_INVALID;
    if(!size)
        return UVDB_MEMORY_TRANSACTION_OK;
    return restore_original(
               io, context, address, scratch, size) ==
               UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED
        ? UVDB_MEMORY_TRANSACTION_OK
        : UVDB_MEMORY_TRANSACTION_RESTORE_PENDING;
}
