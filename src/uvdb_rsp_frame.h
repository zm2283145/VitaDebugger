#pragma once

#include <stddef.h>

enum uvdb_rsp_frame_result {
    UVDB_RSP_FRAME_DISCARD = -1,
    UVDB_RSP_FRAME_INCOMPLETE = 0,
    UVDB_RSP_FRAME_COMPLETE = 1,
};

struct uvdb_rsp_frame {
    size_t payload_offset;
    size_t payload_size;
    size_t consumed_size;
    /* Set only when DISCARD consumed bytes belonging to a malformed frame
     * that began with '$'. ACK-mode receivers must emit one '-' for that
     * disposition; prefix noise is discarded without a NACK. */
    unsigned int request_nack;
};

/* The packet payload returned to the dispatcher is a borrow into the shared
 * receive buffer. Framed replies must not wait for an ACK until that borrow is
 * released, because ACK collection may compact or refill the same buffer. */
struct uvdb_rsp_request_lifetime {
    unsigned int active;
};

/* Scan one bounded input span for an RSP `$PAYLOAD#CC` frame. Bytes before a
 * start marker, superseded starts, oversized payloads, malformed checksums,
 * and checksum mismatches return DISCARD with a nonzero consumed_size.
 * INCOMPLETE never consumes a possible frame. `frame` is unchanged only for
 * invalid arguments; otherwise it receives the parser's exact disposition. */
int uvdb_rsp_scan_frame(
    const void* input,
    size_t input_size,
    size_t maximum_payload_size,
    struct uvdb_rsp_frame* frame);

int uvdb_rsp_frame_should_nack(
    const struct uvdb_rsp_frame* frame,
    int no_ack_mode);

void uvdb_rsp_request_lifetime_init(
    struct uvdb_rsp_request_lifetime* lifetime);
int uvdb_rsp_request_lifetime_begin(
    struct uvdb_rsp_request_lifetime* lifetime);
int uvdb_rsp_request_lifetime_release(
    struct uvdb_rsp_request_lifetime* lifetime);
int uvdb_rsp_request_lifetime_is_active(
    const struct uvdb_rsp_request_lifetime* lifetime);
