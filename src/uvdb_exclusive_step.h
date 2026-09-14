#ifndef UVDB_EXCLUSIVE_STEP_H
#define UVDB_EXCLUSIVE_STEP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These hard ceilings make the scanner bounded even when its caller accepts
 * limits from an untrusted debugger client. Smaller limits are useful when a
 * target wants to cap protected memory reads more aggressively. */
#define UVDB_EXCLUSIVE_STEP_MAX_INSTRUCTIONS 64u
#define UVDB_EXCLUSIVE_STEP_MAX_BYTES 256u

struct uvdb_exclusive_step_limits
{
    unsigned int instruction_limit;
    unsigned int byte_limit;
};

struct uvdb_exclusive_step_target
{
    uint32_t address;
    unsigned int breakpoint_size;
    unsigned int scanned_instructions;
    unsigned int scanned_bytes;
};

/* Return the number of bytes copied, or a negative value on failure. Reads are
 * deliberately limited to one A32 word or one Thumb halfword at a time. */
typedef int (*uvdb_exclusive_step_read_fn)(
    void* context,
    uint32_t address,
    unsigned char* destination,
    size_t size);

/* Scan a straight-line exclusive sequence beginning at pc. cpsr selects A32
 * or Thumb state through T and supplies the condition flags for the first A32
 * LDREX. J, E, ThumbEE, a non-empty Thumb ITSTATE, and misaligned addresses
 * are rejected. The function returns 1 only when target contains the first
 * safe address after a compatible STREX or CLREX; every other result is 0 and
 * leaves target untouched.
 *
 * Both limits must be nonzero and no greater than the hard ceilings above.
 * The instruction limit includes the starting LDREX and the terminator, while
 * the byte limit includes every instruction read through the terminator. */
int uvdb_exclusive_step_scan(
    uvdb_exclusive_step_read_fn read_memory,
    void* read_context,
    uint32_t pc,
    uint32_t cpsr,
    const struct uvdb_exclusive_step_limits* limits,
    struct uvdb_exclusive_step_target* target);

#ifdef __cplusplus
}
#endif

#endif
