#pragma once

#include <stdint.h>

enum uvdb_protocol_gate_result {
    UVDB_PROTOCOL_GATE_INVALID = -2,
    UVDB_PROTOCOL_GATE_CLOSED = -1,
    UVDB_PROTOCOL_GATE_BUSY = 0,
    UVDB_PROTOCOL_GATE_ACQUIRED = 1,
};

/*
 * The RSP receive/output buffers form one protocol session and cannot be
 * shared merely because the global debugger lock is dropped around a blocking
 * socket call. This gate pins that complete session across every recv/send,
 * parser, callback, and buffer resize until its exact owner releases it.
 */
struct uvdb_protocol_gate {
    volatile uint32_t owner;
    volatile uint32_t closing;
};

void uvdb_protocol_gate_init(struct uvdb_protocol_gate* gate);
int uvdb_protocol_gate_try_acquire(
    struct uvdb_protocol_gate* gate,
    uint32_t owner);
int uvdb_protocol_gate_release(
    struct uvdb_protocol_gate* gate,
    uint32_t owner);
void uvdb_protocol_gate_close(struct uvdb_protocol_gate* gate);
int uvdb_protocol_gate_reopen(struct uvdb_protocol_gate* gate);
int uvdb_protocol_gate_is_idle(const struct uvdb_protocol_gate* gate);
int uvdb_protocol_gate_is_closing(const struct uvdb_protocol_gate* gate);
