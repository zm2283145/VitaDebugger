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
