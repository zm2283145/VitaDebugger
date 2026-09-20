#pragma once

#include <stddef.h>
#include <stdint.h>

enum uvdb_memory_transaction_result {
    UVDB_MEMORY_TRANSACTION_OK = 0,
    UVDB_MEMORY_TRANSACTION_INVALID = -1,
    UVDB_MEMORY_TRANSACTION_READ_FAILED = -2,
    UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED = -3,
    UVDB_MEMORY_TRANSACTION_RESTORE_PENDING = -4,
    UVDB_MEMORY_TRANSACTION_STATE_ERROR = -5,
};

struct uvdb_memory_transaction_io {
    size_t (*read)(void* context, uintptr_t address, void* output,
                   size_t size);
    size_t (*write)(void* context, uintptr_t address, const void* input,
                    size_t size);
    int (*sync)(void* context, uintptr_t address, size_t size);
};

enum uvdb_memory_transaction_state {
    UVDB_MEMORY_TRANSACTION_IDLE = 0,
    UVDB_MEMORY_TRANSACTION_PREPARED = 1,
    UVDB_MEMORY_TRANSACTION_APPLIED = 2,
};

struct uvdb_memory_transaction {
    uintptr_t address;
    size_t size;
    unsigned char* original;
    size_t capacity;
    uint8_t state;
};

void uvdb_memory_transaction_init(
    struct uvdb_memory_transaction* transaction,
    void* storage,
    size_t capacity);

/* Snapshot the complete original span and publish a restoration obligation
 * before a caller may mutate the target. */
int uvdb_memory_transaction_prepare(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    size_t size);

/* Apply and verify a prepared write. Failure immediately attempts rollback;
 * RESTORE_PENDING retains the original bytes for an explicit retry. Success
 * remains APPLIED until commit so caller-owned work such as breakpoint
 * re-arming can still fail transactionally. */
int uvdb_memory_transaction_apply(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context,
    const void* input,
    size_t size);

int uvdb_memory_transaction_restore(
    struct uvdb_memory_transaction* transaction,
    const struct uvdb_memory_transaction_io* io,
    void* context);

int uvdb_memory_transaction_commit(
    struct uvdb_memory_transaction* transaction);

int uvdb_memory_transaction_is_pending(
    const struct uvdb_memory_transaction* transaction);

/* Compatibility wrapper for a complete one-shot write. `scratch` must hold
 * the original span for the duration of this call. */
int uvdb_memory_write_transaction(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const void* input,
    size_t size,
    void* scratch,
    size_t scratch_size);
