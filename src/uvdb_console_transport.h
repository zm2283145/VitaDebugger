#pragma once

#include <stddef.h>
#include <stdint.h>

#include "uvdb_console.h"

#define UVDB_CONSOLE_RSP_PAYLOAD_MAX \
    (1u + (2u * UVDB_CONSOLE_RECORD_MAX))
#define UVDB_CONSOLE_RSP_FRAME_MAX \
    (1u + UVDB_CONSOLE_RSP_PAYLOAD_MAX + 3u)

enum uvdb_console_write_result {
    UVDB_CONSOLE_WRITE_ERROR = -1,
    UVDB_CONSOLE_WRITE_COMPLETE = 0,
    UVDB_CONSOLE_WRITE_WOULD_BLOCK = 1,
};

enum uvdb_console_pump_result {
    UVDB_CONSOLE_PUMP_FATAL = -1,
    UVDB_CONSOLE_PUMP_IDLE = 0,
    UVDB_CONSOLE_PUMP_SENT = 1,
    UVDB_CONSOLE_PUMP_BUSY = 2,
    UVDB_CONSOLE_PUMP_WOULD_BLOCK = 3,
    UVDB_CONSOLE_PUMP_COMMITTED = 4,
};

/*
 * Perform exactly one nonblocking write attempt.  COMPLETE is valid only when
 * bytes_sent reports the entire requested size.  WOULD_BLOCK must report zero
 * bytes.  Any other combination is treated as a connection-fatal partial or
 * hard failure because an RSP frame cannot be resumed safely later.
 */
typedef enum uvdb_console_write_result (*uvdb_console_write_fn)(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error);

struct uvdb_console_transport_stats {
    uint32_t frames_sent;
    uint32_t frame_bytes_sent;
    uint32_t would_block;
    uint32_t commit_busy;
    uint32_t partial_writes;
    uint32_t hard_errors;
    uint32_t session_errors;
    int last_native_error;
};

/*
 * Single-owner state for one RSP byte stream.  The owner must serialize every
 * function below with ordinary command/reply traffic on the same socket.
 */
struct uvdb_console_transport {
    uint32_t generation;
    uint64_t sent_uncommitted_sequence;
    uint32_t no_ack_mode;
    uint32_t failed;
    struct uvdb_console_transport_stats stats;
};

void uvdb_console_transport_init(struct uvdb_console_transport* transport);

/* Reset per-connection state and close any previous output generation. */
int uvdb_console_transport_begin_connection(
    struct uvdb_console_transport* transport);

/*
 * Enter no-ack mode only after the framed QStartNoAckMode OK response and its
 * final acknowledgement have completed.  This opens a fresh output generation
 * and is idempotent for repeated requests on the same connection.
 */
int uvdb_console_transport_enable_no_ack(
    struct uvdb_console_transport* transport);

/* Close the generation gate and discard all output belonging to this client. */
int uvdb_console_transport_end_connection(
    struct uvdb_console_transport* transport);

int uvdb_console_transport_no_ack(
    const struct uvdb_console_transport* transport);

/* Send or finish committing at most one record. */
int uvdb_console_transport_pump(
    struct uvdb_console_transport* transport,
    uvdb_console_write_fn write_fn,
    void* write_context);

int uvdb_console_transport_get_stats(
    const struct uvdb_console_transport* transport,
    struct uvdb_console_transport_stats* stats);
