#ifndef UVDB_BREAKPOINT_PATCH_H
#define UVDB_BREAKPOINT_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UVDB_BREAKPOINT_PATCH_MAX_SIZE 4u

enum uvdb_breakpoint_patch_result {
    UVDB_BREAKPOINT_PATCH_OK = 0,
    UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT = -1,
    UVDB_BREAKPOINT_PATCH_ERROR_SLOT_IN_USE = -2,
    UVDB_BREAKPOINT_PATCH_ERROR_ORIGINAL_READ = -3,
    UVDB_BREAKPOINT_PATCH_ERROR_PATCH_WRITE = -4,
    UVDB_BREAKPOINT_PATCH_ERROR_PATCH_SYNC = -5,
    UVDB_BREAKPOINT_PATCH_ERROR_PATCH_READBACK = -6,
    UVDB_BREAKPOINT_PATCH_ERROR_PATCH_MISMATCH = -7,
    UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING = -8,
    UVDB_BREAKPOINT_PATCH_ERROR_IDENTITY = -9,
};

enum uvdb_breakpoint_patch_state {
    UVDB_BREAKPOINT_PATCH_EMPTY = 0,
    /* Original bytes and metadata are valid. The target range might contain
     * original bytes, patch bytes, or a partial mixture; it must be restored
     * and verified before this slot is reused or the target resumes. */
    UVDB_BREAKPOINT_PATCH_RESTORE_PENDING = 1,
    /* Patch bytes were written, cache-synchronized, and read back exactly. */
    UVDB_BREAKPOINT_PATCH_INSTALLED = 2,
};

/* Read/write return the exact number of bytes transferred. A short, zero, or
 * overlong result is an error; the helper never completes a partial operation
 * with a second callback. sync returns zero only after the complete address
 * range is coherent for both data reads and instruction fetch. */
typedef size_t (*uvdb_breakpoint_patch_read_fn)(
    void* user, uintptr_t address, void* output, size_t size);
typedef size_t (*uvdb_breakpoint_patch_write_fn)(
    void* user, uintptr_t address, const void* input, size_t size);
typedef int (*uvdb_breakpoint_patch_sync_fn)(
    void* user, uintptr_t address, size_t size);

struct uvdb_breakpoint_patch_io {
    uvdb_breakpoint_patch_read_fn read;
    uvdb_breakpoint_patch_write_fn write;
    uvdb_breakpoint_patch_sync_fn sync;
    void* user;
};

struct uvdb_breakpoint_patch_identity {
    uint64_t target;
    uint64_t module;
};

/* Return one only while the exact target and module objects named by
 * `identity` still own the address range. Zero or a negative result is a
 * fail-closed mismatch. The caller must serialize object destruction against
 * the callback and following write; numeric IDs alone are not sufficient when
 * an operating system can reuse them. */
typedef int (*uvdb_breakpoint_patch_identity_fn)(
    void* user, uintptr_t address, size_t size,
    const struct uvdb_breakpoint_patch_identity* identity);

struct uvdb_breakpoint_patch_owner {
    struct uvdb_breakpoint_patch_identity identity;
    uvdb_breakpoint_patch_identity_fn matches;
    void* user;
};

/* The slot is caller-owned and allocation-free. Treat its fields as read-only
 * outside this module. Zero initialization is valid; slot_init is provided to
 * make initialization explicit. Operations on one slot are not concurrent. */
struct uvdb_breakpoint_patch_slot {
    uintptr_t address;
    uint8_t original[UVDB_BREAKPOINT_PATCH_MAX_SIZE];
    uint8_t patch[UVDB_BREAKPOINT_PATCH_MAX_SIZE];
    uint8_t size;
    uint8_t state;
    uint8_t identity_bound;
    uint8_t reserved;
    struct uvdb_breakpoint_patch_identity identity;
};

void uvdb_breakpoint_patch_slot_init(
    struct uvdb_breakpoint_patch_slot* slot);

/* Return nonzero whenever the slot still carries a restoration obligation.
 * NULL and unknown states are treated conservatively so lifecycle code never
 * retires an all-stop guard based on unproven breakpoint state. */
int uvdb_breakpoint_patch_requires_restore(
    const struct uvdb_breakpoint_patch_slot* slot);

/* Installation first captures the complete original range. It then records a
 * RESTORE_PENDING obligation in slot before invoking the first write callback.
 * Patch write, cache sync, and readback must all succeed exactly. Any failure
 * triggers one rollback attempt. The original install error is returned when
 * rollback verifies; otherwise RESTORE_PENDING is returned and the slot keeps
 * the recovery metadata needed by a later restore call. */
int uvdb_breakpoint_patch_install(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io, uintptr_t address,
    const void* patch, size_t size);

/* Identity-bound variant for targets whose module/process lifetime can be
 * proven by a retained-object provider. A mismatch before installation writes
 * nothing. A mismatch during restoration retains the complete obligation and
 * writes nothing until the exact owner is available again. */
int uvdb_breakpoint_patch_install_owned(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io,
    const struct uvdb_breakpoint_patch_owner* owner, uintptr_t address,
    const void* patch, size_t size);

/* Restore is idempotent for an empty slot. For installed or pending slots it
 * writes the exact original bytes, synchronizes caches, and reads them back.
 * Only a complete write + successful sync + exact matching readback clears the
 * slot. Any uncertainty leaves RESTORE_PENDING intact for another retry. */
int uvdb_breakpoint_patch_restore(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io);

int uvdb_breakpoint_patch_restore_owned(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io,
    const struct uvdb_breakpoint_patch_owner* owner);

#ifdef __cplusplus
}
#endif

#endif
