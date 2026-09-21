#pragma once

#include <stdint.h>

#define UVDB_SAFETY_GATE_ABI_VERSION UINT32_C(0x00010000)
#define UVDB_SAFETY_GATE_MEMORY_SIZE 192u

enum uvdb_safety_gate_exception_request {
    UVDB_SAFETY_GATE_EXCEPTION_NONE = 0,
    UVDB_SAFETY_GATE_EXCEPTION_DATA_ABORT = 1,
    UVDB_SAFETY_GATE_EXCEPTION_PREFETCH_ABORT = 2,
    UVDB_SAFETY_GATE_EXCEPTION_UNDEFINED_INSTRUCTION = 3,
};

#ifdef UVDB_HARDWARE_SAFETY_GATE
extern volatile uint32_t uvdb_safety_gate_nested_request;
extern volatile uint32_t uvdb_safety_gate_nested_completed;
extern volatile uint32_t uvdb_safety_gate_nested_recovery_pc;
extern volatile uint32_t uvdb_safety_gate_nested_in_flight;

extern volatile uint32_t uvdb_safety_gate_copy_address;
extern volatile uint32_t uvdb_safety_gate_copy_size;
extern volatile uint32_t uvdb_safety_gate_copy_call_count;
extern volatile uint32_t uvdb_safety_gate_copy_fail_first;
extern volatile uint32_t uvdb_safety_gate_copy_fail_count;
#endif
