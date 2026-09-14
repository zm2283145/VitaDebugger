#ifndef UVDB_ASLR_FIXTURE_CONTROL_H
#define UVDB_ASLR_FIXTURE_CONTROL_H

#include <stdint.h>

#define UVDB_ASLR_FIXTURE_ABI_VERSION UINT32_C(0x00010000)
#define UVDB_ASLR_FIXTURE_READY_MAGIC UINT32_C(0x41534c52)
#define UVDB_ASLR_FIXTURE_FAILED_MAGIC UINT32_C(0xffffffff)
#define UVDB_ASLR_MAIN_RESULT_XOR UINT32_C(0x4d41494e)
#define UVDB_ASLR_SUPRX_RESULT_XOR UINT32_C(0x53555052)

/*
 * The application owns this storage for the complete lifetime of the loaded
 * fixture.  Every field is naturally aligned and transferred atomically as a
 * 32-bit word.  Sequence zero is reserved for the idle state.
 */
struct uvdb_aslr_fixture_control {
    uint32_t abi_version;
    uint32_t size;
    volatile uint32_t ready;
    volatile uint32_t request;
    volatile uint32_t acknowledged;
    volatile uint32_t result;
    volatile uint32_t shutdown;
};

/* The module manager copies this small argument block for module_start. */
struct uvdb_aslr_fixture_args {
    uint32_t abi_version;
    uint32_t size;
    struct uvdb_aslr_fixture_control* control;
};

_Static_assert(sizeof(struct uvdb_aslr_fixture_control) == 28,
               "unexpected ASLR fixture control layout");
_Static_assert(sizeof(struct uvdb_aslr_fixture_args) == 12,
               "unexpected ASLR fixture argument layout");

#endif
