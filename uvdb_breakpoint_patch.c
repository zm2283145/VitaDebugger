#include "uvdb_breakpoint_patch.h"

#include <string.h>

static int uvdb_breakpoint_patch_io_is_valid(
    const struct uvdb_breakpoint_patch_io* io)
{
    return io != NULL && io->read != NULL && io->write != NULL &&
           io->sync != NULL;
}

static int uvdb_breakpoint_patch_range_is_valid(uintptr_t address,
                                                size_t size)
{
    return address != 0u && size != 0u &&
           size <= UVDB_BREAKPOINT_PATCH_MAX_SIZE &&
           address <= UINTPTR_MAX - (size - 1u);
}

static int uvdb_breakpoint_patch_slot_metadata_is_valid(
    const struct uvdb_breakpoint_patch_slot* slot)
{
    return slot != NULL &&
           (slot->state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING ||
            slot->state == UVDB_BREAKPOINT_PATCH_INSTALLED) &&
           uvdb_breakpoint_patch_range_is_valid(slot->address, slot->size);
}

static void uvdb_breakpoint_patch_clear(
    struct uvdb_breakpoint_patch_slot* slot)
{
    memset(slot, 0, sizeof(*slot));
}

/* Always exercise all three recovery operations. A partial write may already
 * have changed executable memory, and a failed cache operation does not prove
 * that no cache state changed. Keeping the state pending unless every result is
 * exact is deliberately conservative. */
static int uvdb_breakpoint_patch_restore_internal(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io)
{
    uint8_t readback[UVDB_BREAKPOINT_PATCH_MAX_SIZE] = {0};
    size_t written;
    size_t read;
    int sync_result;

    slot->state = UVDB_BREAKPOINT_PATCH_RESTORE_PENDING;
    written = io->write(io->user, slot->address, slot->original, slot->size);
    sync_result = io->sync(io->user, slot->address, slot->size);
    read = io->read(io->user, slot->address, readback, slot->size);

    if (written != slot->size || sync_result != 0 || read != slot->size ||
        memcmp(readback, slot->original, slot->size) != 0)
        return UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING;

    uvdb_breakpoint_patch_clear(slot);
    return UVDB_BREAKPOINT_PATCH_OK;
}

void uvdb_breakpoint_patch_slot_init(
    struct uvdb_breakpoint_patch_slot* slot)
{
    if (slot != NULL)
        uvdb_breakpoint_patch_clear(slot);
}

int uvdb_breakpoint_patch_requires_restore(
    const struct uvdb_breakpoint_patch_slot* slot)
{
    return slot == NULL || slot->state != UVDB_BREAKPOINT_PATCH_EMPTY;
}

int uvdb_breakpoint_patch_install(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io, uintptr_t address,
    const void* patch, size_t size)
{
    uint8_t desired[UVDB_BREAKPOINT_PATCH_MAX_SIZE] = {0};
    uint8_t original[UVDB_BREAKPOINT_PATCH_MAX_SIZE] = {0};
    uint8_t readback[UVDB_BREAKPOINT_PATCH_MAX_SIZE] = {0};
    int failure;

    if (slot == NULL || patch == NULL ||
        !uvdb_breakpoint_patch_io_is_valid(io) ||
        !uvdb_breakpoint_patch_range_is_valid(address, size) ||
        slot->state > UVDB_BREAKPOINT_PATCH_INSTALLED)
        return UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT;
    if (slot->state != UVDB_BREAKPOINT_PATCH_EMPTY)
        return UVDB_BREAKPOINT_PATCH_ERROR_SLOT_IN_USE;

    memcpy(desired, patch, size);
    if (io->read(io->user, address, original, size) != size)
        return UVDB_BREAKPOINT_PATCH_ERROR_ORIGINAL_READ;

    /* Publish complete recovery metadata before the first byte can change. */
    uvdb_breakpoint_patch_clear(slot);
    slot->address = address;
    memcpy(slot->original, original, size);
    memcpy(slot->patch, desired, size);
    slot->size = (uint8_t)size;
    slot->state = UVDB_BREAKPOINT_PATCH_RESTORE_PENDING;

    if (io->write(io->user, address, slot->patch, size) != size) {
        failure = UVDB_BREAKPOINT_PATCH_ERROR_PATCH_WRITE;
        goto rollback;
    }
    if (io->sync(io->user, address, size) != 0) {
        failure = UVDB_BREAKPOINT_PATCH_ERROR_PATCH_SYNC;
        goto rollback;
    }
    if (io->read(io->user, address, readback, size) != size) {
        failure = UVDB_BREAKPOINT_PATCH_ERROR_PATCH_READBACK;
        goto rollback;
    }
    if (memcmp(readback, slot->patch, size) != 0) {
        failure = UVDB_BREAKPOINT_PATCH_ERROR_PATCH_MISMATCH;
        goto rollback;
    }

    slot->state = UVDB_BREAKPOINT_PATCH_INSTALLED;
    return UVDB_BREAKPOINT_PATCH_OK;

rollback:
    if (uvdb_breakpoint_patch_restore_internal(slot, io) !=
        UVDB_BREAKPOINT_PATCH_OK)
        return UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING;
    return failure;
}

int uvdb_breakpoint_patch_restore(
    struct uvdb_breakpoint_patch_slot* slot,
    const struct uvdb_breakpoint_patch_io* io)
{
    if (slot == NULL)
        return UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT;
    if (slot->state == UVDB_BREAKPOINT_PATCH_EMPTY)
        return UVDB_BREAKPOINT_PATCH_OK;
    if (!uvdb_breakpoint_patch_io_is_valid(io) ||
        !uvdb_breakpoint_patch_slot_metadata_is_valid(slot))
        return UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT;
    return uvdb_breakpoint_patch_restore_internal(slot, io);
}
