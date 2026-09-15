#pragma once

#include <stddef.h>
#include <stdint.h>

#define UVDB_RSP_CORE_REGISTER_COUNT 16
#define UVDB_RSP_VFP_D_REGISTER_COUNT 32
#define UVDB_RSP_LEGACY_FPA_BYTES ((8u * 12u) + 4u)
#define UVDB_RSP_CORE_PACKET_BYTES \
    ((UVDB_RSP_CORE_REGISTER_COUNT * 4u) + UVDB_RSP_LEGACY_FPA_BYTES + 4u)
#define UVDB_RSP_EXPLICIT_CORE_PACKET_BYTES \
    ((UVDB_RSP_CORE_REGISTER_COUNT * 4u) + 4u)
#define UVDB_RSP_VFP_PACKET_BYTES \
    ((UVDB_RSP_VFP_D_REGISTER_COUNT * 8u) + 4u)
#define UVDB_RSP_CORE_PACKET_HEX_SIZE (UVDB_RSP_CORE_PACKET_BYTES * 2u)
#define UVDB_RSP_EXPLICIT_CORE_PACKET_HEX_SIZE \
    (UVDB_RSP_EXPLICIT_CORE_PACKET_BYTES * 2u)
#define UVDB_RSP_VFP_PACKET_HEX_SIZE \
    ((UVDB_RSP_EXPLICIT_CORE_PACKET_BYTES + UVDB_RSP_VFP_PACKET_BYTES) * 2u)

#define UVDB_RSP_REGISTER_CORE_FIRST 0u
#define UVDB_RSP_REGISTER_CORE_LAST 15u
#define UVDB_RSP_REGISTER_LEGACY_FPA_FIRST 16u
#define UVDB_RSP_REGISTER_LEGACY_FPA_LAST 23u
#define UVDB_RSP_REGISTER_LEGACY_FPS 24u
#define UVDB_RSP_REGISTER_CPSR 25u
#define UVDB_RSP_REGISTER_VFP_D_FIRST 26u
#define UVDB_RSP_REGISTER_VFP_D_LAST \
    (UVDB_RSP_REGISTER_VFP_D_FIRST + UVDB_RSP_VFP_D_REGISTER_COUNT - 1u)
#define UVDB_RSP_REGISTER_VFP_FPSCR \
    (UVDB_RSP_REGISTER_VFP_D_LAST + 1u)

enum uvdb_rsp_register_result {
    UVDB_RSP_REGISTER_OK = 0,
    UVDB_RSP_REGISTER_INVALID = -1,
    UVDB_RSP_REGISTER_UNSUPPORTED = -2,
};

struct uvdb_rsp_core_registers {
    uint32_t r[UVDB_RSP_CORE_REGISTER_COUNT];
    uint32_t cpsr;
};

struct uvdb_rsp_vfp_registers {
    uint64_t d[UVDB_RSP_VFP_D_REGISTER_COUNT];
    uint32_t fpscr;
};

/* A validated write to one mutable ARM core register. VFP and legacy FPA
 * registers are intentionally excluded: their current snapshots are read
 * only and have no safe restoration ABI. */
struct uvdb_rsp_core_register_write {
    uint32_t register_number;
    uint32_t value;
};

/* A fully validated ARM32 memory request. `data` points into the caller's
 * packet and is populated only for `M` writes. Parser outputs are left
 * untouched on every failure. */
struct uvdb_rsp_memory_request {
    uint32_t address;
    size_t size;
    const char* data;
};

/* One validated qXfer OFFSET,LENGTH suffix. */
struct uvdb_rsp_xfer_range {
    uint64_t offset;
    uint64_t length;
};

/*
 * Encode one GDB remote-console payload: the literal 'O' followed by two
 * lowercase hexadecimal characters for every input byte.  The RSP '$...#cc'
 * framing is intentionally left to the transport.  output_size is required
 * and receives the required payload size even when capacity is insufficient.
 * The output is not NUL terminated.  A NULL data pointer is valid only when
 * data_size is zero.  Input and output spans must not overlap.
 */
int uvdb_rsp_encode_console_payload(
    char* output,
    size_t capacity,
    const void* data,
    size_t data_size,
    size_t* output_size);

// Encode GDB's ARM register packet in target-description register-number
// order. The legacy ARM description includes unavailable FPA slots 16-24.
// The explicit D32 description omits those undescribed slots from `g`, places
// CPSR after PC, and then emits D0-D31 plus FPSCR. A NULL VFP snapshot emits
// unavailable markers while preserving the negotiated explicit packet shape.
//
// `output_size` is required and receives the required size even when capacity
// is insufficient. The output is not NUL terminated.
int uvdb_rsp_encode_register_packet(
    char* output,
    size_t capacity,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp,
    int include_vfp,
    size_t* output_size);

/* Parse a complete `pN` request. N is hexadecimal and must name a register in
 * the negotiated layout: 0 selects legacy ARM, 1 selects explicit ARM+D32. */
int uvdb_rsp_parse_register_read_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    uint32_t* register_number);

/* Encode one register in the same little-endian form used by `g`. Valid
 * unavailable registers are encoded as correctly sized `xx` markers.
 * output_size receives the required byte count even on short capacity. */
int uvdb_rsp_encode_single_register(
    char* output,
    size_t capacity,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp,
    int include_vfp,
    uint32_t register_number,
    size_t* output_size);

/* Parse a complete `PN=VALUE` request for one mutable core register. VALUE is
 * exactly eight hex characters in target little-endian order. Recognized
 * VFP/FPA writes return UVDB_RSP_REGISTER_UNSUPPORTED. `write` is unchanged
 * on failure. */
int uvdb_rsp_parse_core_register_write_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    struct uvdb_rsp_core_register_write* write);

/* Apply a validated write. If requested, inverse receives a write restoring
 * the prior value. Stop-session ownership remains the live caller's duty. */
int uvdb_rsp_apply_core_register_write(
    struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_core_register_write* write,
    struct uvdb_rsp_core_register_write* inverse);

/* Parse a complete legacy `G` packet transactionally. The unavailable FPA
 * region may contain hexadecimal bytes or `xx` byte markers, but every core
 * register and CPSR must be present and hexadecimal. `core` is unchanged on
 * malformed or truncated input. The explicit VFP layout remains unsupported
 * until a restorable full-bank setter exists. */
int uvdb_rsp_parse_core_register_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    struct uvdb_rsp_core_registers* core);

/* Strict ARM32 `mADDR,LENGTH` and `MADDR,LENGTH:HEX` parsers. Addresses and
 * lengths are nonempty hexadecimal fields, the range must not wrap the
 * 32-bit address space, and size must not exceed `maximum_size`. For `M`, the
 * payload must contain exactly two hexadecimal digits per byte. */
int uvdb_rsp_parse_memory_read_packet(
    const char* packet,
    size_t packet_size,
    size_t maximum_size,
    struct uvdb_rsp_memory_request* request);
int uvdb_rsp_parse_memory_write_packet(
    const char* packet,
    size_t packet_size,
    size_t maximum_size,
    struct uvdb_rsp_memory_request* request);

/* Decode an exact hexadecimal byte span after first validating the complete
 * input. Destination bytes are never partially changed on malformed input.
 * Input/output overlap is rejected. */
int uvdb_rsp_decode_hex_bytes(
    void* output,
    size_t output_size,
    const char* text,
    size_t text_size);

/* Parse a complete `Z0,ADDR,KIND` or `z0,ADDR,KIND` software-breakpoint
 * packet. Outputs are unchanged on failure. */
int uvdb_rsp_parse_software_breakpoint_packet(
    const char* packet,
    size_t packet_size,
    int* insert,
    uint32_t* address,
    size_t* kind);

/* Parse an exact qXfer OFFSET,LENGTH suffix. Each field and the resulting
 * half-open range must fit in uint64_t. */
int uvdb_rsp_parse_xfer_range(
    const char* text,
    size_t text_size,
    struct uvdb_rsp_xfer_range* range);

struct uvdb_rsp_fileio_result {
    uint32_t result;
    uint32_t error_number;
    int has_error_number;
    int interrupted;
    int has_attachment;
    const char* attachment;
    size_t attachment_size;
};

/* Parse a complete remote file-I/O reply
 * (`FRESULT[,ERRNO[,C]][;ATTACHMENT]`). RESULT may have one leading minus
 * sign; numeric fields are bounded hexadecimal and the Ctrl-C field is the
 * protocol's literal `C`. The attachment is a bounded view into `packet` and
 * may contain arbitrary bytes. Output remains unchanged on failure. */
int uvdb_rsp_parse_fileio_packet(
    const char* packet,
    size_t packet_size,
    struct uvdb_rsp_fileio_result* result);

/* Compatibility wrapper for callers interested only in RESULT. */
int uvdb_rsp_parse_fileio_result_packet(
    const char* packet,
    size_t packet_size,
    uint32_t* result);
