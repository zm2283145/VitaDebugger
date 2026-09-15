#pragma once

#include <stddef.h>
#include <stdint.h>

enum uvdb_memory_transaction_result {
    UVDB_MEMORY_TRANSACTION_OK = 0,
    UVDB_MEMORY_TRANSACTION_INVALID = -1,
    UVDB_MEMORY_TRANSACTION_READ_FAILED = -2,
    UVDB_MEMORY_TRANSACTION_WRITE_FAILED_RESTORED = -3,
    UVDB_MEMORY_TRANSACTION_RESTORE_PENDING = -4,
};

struct uvdb_memory_transaction_io {
    size_t (*read)(void* context, uintptr_t address, void* output,
                   size_t size);
    size_t (*write)(void* context, uintptr_t address, const void* input,
                    size_t size);
    int (*sync)(void* context, uintptr_t address, size_t size);
};

/* Apply one bounded write with exact original-byte rollback. `scratch` must
 * hold the complete original span. The helper first snapshots all bytes,
 * applies and verifies the full write, then restores and verifies the original
 * on any failure. RESTORE_PENDING means the caller must retain the scratch
 * bytes and a coherent all-stop until it can retry restoration. */
int uvdb_memory_write_transaction(
    const struct uvdb_memory_transaction_io* io,
    void* context,
    uintptr_t address,
    const void* input,
    size_t size,
    void* scratch,
    size_t scratch_size);
