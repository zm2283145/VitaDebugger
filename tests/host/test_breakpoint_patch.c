#include "uvdb_breakpoint_patch.h"

#include <stdio.h>
#include <string.h>

#define FAKE_BASE ((uintptr_t)0x1000u)
#define FAKE_OPERATION_LIMIT 16u
#define FAKE_EXACT ((size_t)-1)

static int failures;

static void check(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct fake_memory {
    uint8_t bytes[16];
    size_t read_returns[FAKE_OPERATION_LIMIT];
    size_t write_returns[FAKE_OPERATION_LIMIT];
    uint8_t read_xor[FAKE_OPERATION_LIMIT];
    int sync_results[FAKE_OPERATION_LIMIT];
    unsigned int read_calls;
    unsigned int write_calls;
    unsigned int sync_calls;
    unsigned int writes_while_pending;
    unsigned int writes_without_obligation;
    const struct uvdb_breakpoint_patch_slot* observed_slot;
    struct uvdb_breakpoint_patch_identity identity;
    int connected;
};

static void fake_init(struct fake_memory* memory)
{
    unsigned int i;
    memset(memory, 0, sizeof(*memory));
    for (i = 0u; i < sizeof(memory->bytes); ++i)
        memory->bytes[i] = (uint8_t)(0x20u + i);
    for (i = 0u; i < FAKE_OPERATION_LIMIT; ++i) {
        memory->read_returns[i] = FAKE_EXACT;
        memory->write_returns[i] = FAKE_EXACT;
    }
    memory->identity.target = UINT64_C(0x1122334455667788);
    memory->identity.module = UINT64_C(0x8877665544332211);
    memory->connected = 1;
}

static int fake_range(uintptr_t address, size_t size, size_t* offset)
{
    if (address < FAKE_BASE || size > 16u ||
        address - FAKE_BASE > 16u - size)
        return 0;
    *offset = (size_t)(address - FAKE_BASE);
    return 1;
}

static size_t fake_read(void* user, uintptr_t address, void* output,
                        size_t size)
{
    struct fake_memory* memory = (struct fake_memory*)user;
    unsigned int call = memory->read_calls++;
    size_t offset;
    size_t reported;
    size_t copied;

    if (!memory->connected || call >= FAKE_OPERATION_LIMIT || output == NULL ||
        !fake_range(address, size, &offset))
        return 0u;
    reported = memory->read_returns[call] == FAKE_EXACT
                   ? size
                   : memory->read_returns[call];
    copied = reported < size ? reported : size;
    if (copied != 0u)
        memcpy(output, memory->bytes + offset, copied);
    if (copied != 0u && memory->read_xor[call] != 0u)
        ((uint8_t*)output)[0] ^= memory->read_xor[call];
    return reported;
}

static size_t fake_write(void* user, uintptr_t address, const void* input,
                         size_t size)
{
    struct fake_memory* memory = (struct fake_memory*)user;
    unsigned int call = memory->write_calls++;
    size_t offset;
    size_t reported;
    size_t copied;

    if (memory->observed_slot != NULL &&
        memory->observed_slot->state ==
            UVDB_BREAKPOINT_PATCH_RESTORE_PENDING)
        ++memory->writes_while_pending;
    else
        ++memory->writes_without_obligation;
    if (!memory->connected || call >= FAKE_OPERATION_LIMIT || input == NULL ||
        !fake_range(address, size, &offset))
        return 0u;
    reported = memory->write_returns[call] == FAKE_EXACT
                   ? size
                   : memory->write_returns[call];
    copied = reported < size ? reported : size;
    if (copied != 0u)
        memcpy(memory->bytes + offset, input, copied);
    return reported;
}

static int fake_sync(void* user, uintptr_t address, size_t size)
{
    struct fake_memory* memory = (struct fake_memory*)user;
    unsigned int call = memory->sync_calls++;
    size_t offset;
    if (call >= FAKE_OPERATION_LIMIT ||
        !fake_range(address, size, &offset))
        return -1;
    (void)offset;
    return memory->sync_results[call];
}

static struct uvdb_breakpoint_patch_io fake_io(struct fake_memory* memory)
{
    struct uvdb_breakpoint_patch_io io;
    io.read = fake_read;
    io.write = fake_write;
    io.sync = fake_sync;
    io.user = memory;
    return io;
}

static int fake_identity_matches(
    void* user, uintptr_t address, size_t size,
    const struct uvdb_breakpoint_patch_identity* identity)
{
    struct fake_memory* memory = (struct fake_memory*)user;
    size_t offset;
    return memory->connected && identity != NULL &&
           fake_range(address, size, &offset) &&
           memcmp(identity, &memory->identity, sizeof(*identity)) == 0;
}

static struct uvdb_breakpoint_patch_owner fake_owner(
    struct fake_memory* memory)
{
    struct uvdb_breakpoint_patch_owner owner;
    owner.identity = memory->identity;
    owner.matches = fake_identity_matches;
    owner.user = memory;
    return owner;
}

static int slot_is_clear(const struct uvdb_breakpoint_patch_slot* slot)
{
    struct uvdb_breakpoint_patch_slot empty;
    memset(&empty, 0, sizeof(empty));
    return memcmp(slot, &empty, sizeof(empty)) == 0;
}

static void test_success_and_idempotent_restore(void)
{
    static const uint8_t thumb_trap[2] = {0x00u, 0xdeu};
    static const uint8_t arm_trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    uint8_t original[4];
    unsigned int writes;

    fake_init(&memory);
    io = fake_io(&memory);
    memset(&slot, 0xa5, sizeof(slot));
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes + 4u, 2u);
    check(!uvdb_breakpoint_patch_requires_restore(&slot),
          "initialized slot has no restoration obligation");

    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE + 4u,
                                        thumb_trap,
                                        sizeof(thumb_trap)) ==
              UVDB_BREAKPOINT_PATCH_OK,
          "install a two-byte Thumb patch");
    check(slot.state == UVDB_BREAKPOINT_PATCH_INSTALLED &&
              slot.address == FAKE_BASE + 4u && slot.size == 2u &&
              memcmp(slot.original, original, 2u) == 0 &&
              memcmp(slot.patch, thumb_trap, 2u) == 0,
          "successful install retains exact recovery metadata");
    check(uvdb_breakpoint_patch_requires_restore(&slot),
          "installed patch keeps the lifecycle guard active");
    check(memcmp(memory.bytes + 4u, thumb_trap, 2u) == 0,
          "successful install writes trap bytes");
    check(memory.read_calls == 2u && memory.write_calls == 1u &&
              memory.sync_calls == 1u &&
              memory.writes_while_pending == 1u &&
              memory.writes_without_obligation == 0u,
          "restoration obligation is visible before the first write");

    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
              UVDB_BREAKPOINT_PATCH_OK,
          "restore a verified Thumb patch");
    check(memcmp(memory.bytes + 4u, original, 2u) == 0 &&
              slot_is_clear(&slot),
          "exact restoration verification clears the slot");
    check(!uvdb_breakpoint_patch_requires_restore(&slot),
          "verified restoration permits lifecycle guard retirement");
    writes = memory.write_calls;
    check(uvdb_breakpoint_patch_restore(&slot, NULL) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              memory.write_calls == writes,
          "restoring an empty slot is idempotent and needs no callbacks");

    memcpy(original, memory.bytes + 8u, sizeof(original));
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE + 8u,
                                        arm_trap, sizeof(arm_trap)) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              memcmp(memory.bytes + 8u, arm_trap, sizeof(arm_trap)) == 0,
          "maximum four-byte ARM patch installs");
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              memcmp(memory.bytes + 8u, original, sizeof(original)) == 0,
          "maximum four-byte ARM patch restores");
    check(memory.writes_without_obligation == 0u,
          "every successful-path write observes a recovery obligation");
}

static void test_partial_original_read_never_writes(void)
{
    static const uint8_t trap[2] = {0x00u, 0xdeu};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    uint8_t before[16];

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(before, memory.bytes, sizeof(before));
    memory.read_returns[0] = 1u;

    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
              UVDB_BREAKPOINT_PATCH_ERROR_ORIGINAL_READ,
          "partial original read rejects installation");
    check(slot_is_clear(&slot) &&
              memcmp(memory.bytes, before, sizeof(before)) == 0 &&
              memory.write_calls == 0u && memory.sync_calls == 0u,
          "failed original read creates no obligation and writes nothing");
}

static void test_install_failures_roll_back(void)
{
    static const uint8_t trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    uint8_t original[4];

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    memory.write_returns[0] = 2u;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_PATCH_WRITE &&
              slot_is_clear(&slot) &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "partial patch write rolls back and reports its stage");
    check(memory.write_calls == 2u && memory.sync_calls == 1u &&
              memory.read_calls == 2u &&
              memory.writes_while_pending == 2u,
          "partial write rollback is synchronized and verified");

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    memory.sync_results[0] = -1;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_PATCH_SYNC &&
              slot_is_clear(&slot) &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "patch cache-sync failure rolls back");
    check(memory.write_calls == 2u && memory.sync_calls == 2u &&
              memory.read_calls == 2u,
          "sync failure still performs a complete rollback transaction");

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    memory.read_returns[1] = 2u;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_PATCH_READBACK &&
              slot_is_clear(&slot) &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "partial patch readback rolls back");

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    memory.read_xor[1] = 1u;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_PATCH_MISMATCH &&
              slot_is_clear(&slot) &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "trap-byte mismatch rolls back");
    check(memory.writes_without_obligation == 0u,
          "all rollback paths mark the obligation before writing");
}

static void test_failed_rollback_then_retry(void)
{
    static const uint8_t trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    uint8_t original[4];
    unsigned int reads;
    unsigned int writes;
    unsigned int syncs;

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));

    /* Force installed-byte verification to fail, then make the automatic
       rollback write only half of the original instruction. */
    memory.read_xor[1] = 1u;
    memory.write_returns[1] = 2u;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
              UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING,
          "unverified rollback reports restore pending");
    check(slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              slot.address == FAKE_BASE && slot.size == sizeof(trap) &&
              memcmp(slot.original, original, sizeof(original)) == 0 &&
              memcmp(slot.patch, trap, sizeof(trap)) == 0,
          "failed rollback retains complete recovery metadata");
    check(uvdb_breakpoint_patch_requires_restore(&slot),
          "failed rollback keeps the lifecycle guard active");
    check(memcmp(memory.bytes, original, 2u) == 0 &&
              memcmp(memory.bytes + 2u, trap + 2u, 2u) == 0,
          "partial rollback leaves an explicitly tracked mixed instruction");

    reads = memory.read_calls;
    writes = memory.write_calls;
    syncs = memory.sync_calls;
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE + 4u, trap,
                                        sizeof(trap)) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_SLOT_IN_USE &&
              memory.read_calls == reads && memory.write_calls == writes &&
              memory.sync_calls == syncs,
          "pending slot cannot be overwritten by another install");

    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              memcmp(memory.bytes, original, sizeof(original)) == 0 &&
              slot_is_clear(&slot),
          "later exact retry restores and clears a pending slot");
    check(!uvdb_breakpoint_patch_requires_restore(&slot),
          "successful retry permits lifecycle guard retirement");
    check(memory.writes_without_obligation == 0u &&
              memory.writes_while_pending == 3u,
          "retry also writes only after observing pending state");
}

static void test_restore_requires_sync_and_exact_verification(void)
{
    static const uint8_t trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    uint8_t original[4];

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
              UVDB_BREAKPOINT_PATCH_OK,
          "prepare installed patch for restore-sync test");
    memory.sync_results[1] = -1;
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "failed restore sync retains obligation even when data reads back");
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              slot_is_clear(&slot),
          "restore succeeds after cache-sync retry");

    fake_init(&memory);
    io = fake_io(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap,
                                        sizeof(trap)) ==
              UVDB_BREAKPOINT_PATCH_OK,
          "prepare installed patch for restore-readback test");
    memory.read_xor[2] = 1u;
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              memcmp(memory.bytes, original, sizeof(original)) == 0,
          "mismatched restore readback cannot clear recovery metadata");
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
                  UVDB_BREAKPOINT_PATCH_OK &&
              slot_is_clear(&slot),
          "exact restoration retry clears after prior mismatch");
}

static void test_disconnect_competing_owner_and_identity_retry(void)
{
    static const uint8_t trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    struct uvdb_breakpoint_patch_owner owner;
    struct uvdb_breakpoint_patch_owner stale_owner;
    uint8_t original[4];
    unsigned int writes;

    fake_init(&memory);
    io = fake_io(&memory);
    owner = fake_owner(&memory);
    uvdb_breakpoint_patch_slot_init(&slot);
    memory.observed_slot = &slot;
    memcpy(original, memory.bytes, sizeof(original));

    check(uvdb_breakpoint_patch_install_owned(
              &slot, &io, &owner, FAKE_BASE, trap, sizeof(trap)) ==
              UVDB_BREAKPOINT_PATCH_OK &&
              slot.identity_bound == 1u &&
              memcmp(&slot.identity, &owner.identity,
                     sizeof(slot.identity)) == 0,
          "owned install retains exact target and module identity");

    stale_owner = owner;
    stale_owner.identity.module++;
    writes = memory.write_calls;
    check(uvdb_breakpoint_patch_install_owned(
              &slot, &io, &stale_owner, FAKE_BASE + 4u, trap,
              sizeof(trap)) == UVDB_BREAKPOINT_PATCH_ERROR_SLOT_IN_USE &&
              memory.write_calls == writes,
          "competing owner cannot replace an occupied trap slot");

    memory.connected = 0;
    check(uvdb_breakpoint_patch_restore_owned(&slot, &io, &owner) ==
              UVDB_BREAKPOINT_PATCH_ERROR_IDENTITY &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              memory.write_calls == writes,
          "disconnect retains restoration metadata without writing");

    memory.connected = 1;
    memory.identity.module++;
    check(uvdb_breakpoint_patch_restore_owned(&slot, &io, &owner) ==
              UVDB_BREAKPOINT_PATCH_ERROR_IDENTITY &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              memory.write_calls == writes,
          "stale module identity cannot restore into a replacement mapping");
    memory.identity.module--;
    memory.identity.target++;
    check(uvdb_breakpoint_patch_restore_owned(&slot, &io, &owner) ==
              UVDB_BREAKPOINT_PATCH_ERROR_IDENTITY &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
              memory.write_calls == writes,
          "stale target identity cannot restore into a replacement process");

    memory.identity = owner.identity;
    check(uvdb_breakpoint_patch_restore_owned(&slot, &io, &owner) ==
              UVDB_BREAKPOINT_PATCH_OK &&
              memcmp(memory.bytes, original, sizeof(original)) == 0 &&
              slot_is_clear(&slot),
          "matching retained identities authorize exact restoration retry");
}

static void test_invalid_arguments(void)
{
    static const uint8_t trap[4] = {0xf0u, 0x00u, 0xf0u, 0xe7u};
    struct uvdb_breakpoint_patch_slot slot;
    struct fake_memory memory;
    struct uvdb_breakpoint_patch_io io;
    struct uvdb_breakpoint_patch_io incomplete;

    fake_init(&memory);
    io = fake_io(&memory);
    incomplete = io;
    uvdb_breakpoint_patch_slot_init(NULL);
    uvdb_breakpoint_patch_slot_init(&slot);

    check(uvdb_breakpoint_patch_install(NULL, &io, FAKE_BASE, trap, 4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects NULL slot");
    check(uvdb_breakpoint_patch_install(&slot, NULL, FAKE_BASE, trap, 4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects NULL callbacks");
    incomplete.read = NULL;
    check(uvdb_breakpoint_patch_install(&slot, &incomplete, FAKE_BASE, trap,
                                        4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install requires read callback");
    incomplete = io;
    incomplete.write = NULL;
    check(uvdb_breakpoint_patch_install(&slot, &incomplete, FAKE_BASE, trap,
                                        4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install requires write callback");
    incomplete = io;
    incomplete.sync = NULL;
    check(uvdb_breakpoint_patch_install(&slot, &incomplete, FAKE_BASE, trap,
                                        4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install requires cache-sync callback");
    check(uvdb_breakpoint_patch_install(&slot, &io, 0u, trap, 4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects zero address");
    check(uvdb_breakpoint_patch_install(&slot, &io, UINTPTR_MAX - 1u, trap,
                                        4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects wrapped address range");
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap, 0u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects zero size");
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap, 5u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects patches larger than four bytes");
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, NULL, 4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects NULL patch bytes");
    check(memory.read_calls == 0u && memory.write_calls == 0u &&
              memory.sync_calls == 0u,
          "invalid installs invoke no callbacks");

    slot.state = 99u;
    check(uvdb_breakpoint_patch_requires_restore(&slot) &&
              uvdb_breakpoint_patch_requires_restore(NULL),
          "unknown and missing slot state keep lifecycle fail-closed");
    check(uvdb_breakpoint_patch_install(&slot, &io, FAKE_BASE, trap, 4u) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "install rejects corrupt slot state");
    check(uvdb_breakpoint_patch_restore(&slot, &io) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "restore rejects corrupt slot state");
    uvdb_breakpoint_patch_slot_init(&slot);
    check(uvdb_breakpoint_patch_restore(NULL, &io) ==
              UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT,
          "restore rejects NULL slot");
    check(uvdb_breakpoint_patch_restore(&slot, NULL) ==
              UVDB_BREAKPOINT_PATCH_OK,
          "empty restore remains idempotent with no I/O object");

    slot.address = FAKE_BASE;
    slot.size = 4u;
    memcpy(slot.original, memory.bytes, 4u);
    slot.state = UVDB_BREAKPOINT_PATCH_RESTORE_PENDING;
    check(uvdb_breakpoint_patch_restore(&slot, NULL) ==
                  UVDB_BREAKPOINT_PATCH_ERROR_INVALID_ARGUMENT &&
              slot.state == UVDB_BREAKPOINT_PATCH_RESTORE_PENDING,
          "active restore requires callbacks and preserves obligation");
}

int main(void)
{
    test_success_and_idempotent_restore();
    test_partial_original_read_never_writes();
    test_install_failures_roll_back();
    test_failed_rollback_then_retry();
    test_restore_requires_sync_and_exact_verification();
    test_disconnect_competing_owner_and_identity_retry();
    test_invalid_arguments();

    if (failures != 0) {
        fprintf(stderr, "%d breakpoint patch test(s) failed\n", failures);
        return 1;
    }
    puts("breakpoint patch transaction tests passed");
    return 0;
}
