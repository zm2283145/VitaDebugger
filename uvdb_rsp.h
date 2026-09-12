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

struct uvdb_rsp_core_registers {
    uint32_t r[UVDB_RSP_CORE_REGISTER_COUNT];
    uint32_t cpsr;
};

struct uvdb_rsp_vfp_registers {
    uint64_t d[UVDB_RSP_VFP_D_REGISTER_COUNT];
    uint32_t fpscr;
};

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
