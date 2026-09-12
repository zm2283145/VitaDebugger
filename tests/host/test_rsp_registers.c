#include <stdio.h>
#include <string.h>

#include "uvdb_rsp.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int span_is(const char* data, size_t size, char value)
{
    for(size_t i = 0; i < size; ++i)
        if(data[i] != value)
            return 0;
    return 1;
}

int main(void)
{
    struct uvdb_rsp_core_registers core = {0};
    for(unsigned int i = 0; i < UVDB_RSP_CORE_REGISTER_COUNT; ++i)
        core.r[i] = 0x11223300u + i;
    core.cpsr = 0xA1B2C3D4u;

    struct uvdb_rsp_vfp_registers vfp = {0};
    vfp.d[0] = 0x0123456789ABCDEFULL;
    vfp.d[31] = 0xFEDCBA9876543210ULL;
    vfp.fpscr = 0x00400000u;

    char packet[UVDB_RSP_VFP_PACKET_HEX_SIZE + 1];
    memset(packet, '#', sizeof(packet));
    size_t size = 0;
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), &core,
                                           NULL, 0, &size) == 0,
          "encode core packet");
    check(size == UVDB_RSP_CORE_PACKET_HEX_SIZE,
          "core packet size");
    check(memcmp(packet, "00332211", 8) == 0,
          "r0 little-endian encoding");
    check(memcmp(packet + 15 * 8, "0f332211", 8) == 0,
          "pc slot encoding");
    check(span_is(packet + 16 * 8,
                  UVDB_RSP_LEGACY_FPA_BYTES * 2, 'x'),
          "legacy FPA slots unavailable");
    check(memcmp(packet + 16 * 8 + UVDB_RSP_LEGACY_FPA_BYTES * 2,
                 "d4c3b2a1", 8) == 0,
          "cpsr register 25 encoding");
    check(packet[size] == '#', "encoder does not append terminator");

    memset(packet, '#', sizeof(packet));
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), &core,
                                           &vfp, 1, &size) == 0,
          "encode VFP packet");
    check(size == UVDB_RSP_VFP_PACKET_HEX_SIZE,
          "VFP packet size");
    check(memcmp(packet + UVDB_RSP_EXPLICIT_CORE_PACKET_HEX_SIZE,
                 "efcdab8967452301", 16) == 0,
          "d0 little-endian encoding");
    check(memcmp(packet + UVDB_RSP_EXPLICIT_CORE_PACKET_HEX_SIZE + 31 * 16,
                 "1032547698badcfe", 16) == 0,
          "d31 little-endian encoding");
    check(memcmp(packet + UVDB_RSP_VFP_PACKET_HEX_SIZE - 8,
                 "00004000", 8) == 0,
          "fpscr encoding");

    memset(packet, '#', sizeof(packet));
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), &core,
                                           NULL, 1, &size) == 0,
          "encode unavailable VFP packet");
    check(span_is(packet + UVDB_RSP_EXPLICIT_CORE_PACKET_HEX_SIZE,
                  UVDB_RSP_VFP_PACKET_BYTES * 2, 'x'),
          "unavailable VFP slots preserve packet shape");

    size = 0;
    check(uvdb_rsp_encode_register_packet(
              packet, UVDB_RSP_VFP_PACKET_HEX_SIZE - 1, &core, &vfp, 1,
              &size) < 0 && size == UVDB_RSP_VFP_PACKET_HEX_SIZE,
          "short capacity fails with required size");
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), NULL,
                                           &vfp, 1, &size) < 0,
          "NULL core rejected");
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), &core,
                                           &vfp, 1, NULL) < 0,
          "NULL output size rejected");

    if(failures)
        return 1;
    puts("PASS: ARM/VFP RSP register serialization");
    return 0;
}
