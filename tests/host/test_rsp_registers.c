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

static void test_individual_read_packets(
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp)
{
    uint32_t number = UINT32_C(0xfeedbeef);
    check(uvdb_rsp_parse_register_read_packet("p0", 2, 0, &number) == 0 &&
              number == 0,
          "parse legacy r0 read");
    check(uvdb_rsp_parse_register_read_packet("pF", 2, 1, &number) == 0 &&
              number == 15,
          "parse uppercase PC read");
    check(uvdb_rsp_parse_register_read_packet("p19", 3, 0, &number) == 0 &&
              number == UVDB_RSP_REGISTER_CPSR,
          "parse legacy CPSR read");
    check(uvdb_rsp_parse_register_read_packet("p1a", 3, 1, &number) == 0 &&
              number == UVDB_RSP_REGISTER_VFP_D_FIRST,
          "parse negotiated D0 read");
    check(uvdb_rsp_parse_register_read_packet("p39", 3, 1, &number) == 0 &&
              number == UVDB_RSP_REGISTER_VFP_D_LAST,
          "parse negotiated D31 read");
    check(uvdb_rsp_parse_register_read_packet("p3a", 3, 1, &number) == 0 &&
              number == UVDB_RSP_REGISTER_VFP_FPSCR,
          "parse negotiated FPSCR read");
    check(uvdb_rsp_parse_register_read_packet("p10", 3, 0, &number) == 0 &&
              number == UVDB_RSP_REGISTER_LEGACY_FPA_FIRST,
          "legacy FPA register is described");

    number = UINT32_C(0xfeedbeef);
    check(uvdb_rsp_parse_register_read_packet("p10", 3, 1, &number) ==
              UVDB_RSP_REGISTER_INVALID &&
              number == UINT32_C(0xfeedbeef),
          "VFP layout rejects legacy register-number gap");
    check(uvdb_rsp_parse_register_read_packet("p1a", 3, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "legacy layout rejects D0");
    check(uvdb_rsp_parse_register_read_packet("p3b", 3, 1, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "VFP layout rejects register after FPSCR");
    check(uvdb_rsp_parse_register_read_packet("p", 1, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "empty register number rejected");
    check(uvdb_rsp_parse_register_read_packet("q0", 2, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "wrong read command rejected");
    check(uvdb_rsp_parse_register_read_packet("p-1", 3, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "signed register number rejected");
    check(uvdb_rsp_parse_register_read_packet("p0x", 3, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "trailing register garbage rejected");
    check(uvdb_rsp_parse_register_read_packet("p100000000", 10, 1,
                                               &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "overflow-width register number rejected");
    check(uvdb_rsp_parse_register_read_packet("p00000000f", 10, 1,
                                               &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "overlong leading-zero register number rejected");
    check(uvdb_rsp_parse_register_read_packet("p0", 2, 2, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "unknown target layout rejected");
    check(uvdb_rsp_parse_register_read_packet(NULL, 2, 0, &number) ==
              UVDB_RSP_REGISTER_INVALID,
          "NULL read packet rejected");
    check(uvdb_rsp_parse_register_read_packet("p0", 2, 0, NULL) ==
              UVDB_RSP_REGISTER_INVALID,
          "NULL read result rejected");

    char encoded[32];
    size_t encoded_size = 0;
    memset(encoded, '#', sizeof(encoded));
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 0, 0, &encoded_size) == 0 &&
              encoded_size == 8 && memcmp(encoded, "00332211", 8) == 0,
          "single r0 matches g packet encoding");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_CPSR, &encoded_size) == 0 &&
              encoded_size == 8 && memcmp(encoded, "d4c3b2a1", 8) == 0,
          "single CPSR little-endian encoding");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 0,
              UVDB_RSP_REGISTER_LEGACY_FPA_FIRST, &encoded_size) == 0 &&
              encoded_size == 24 && span_is(encoded, 24, 'x'),
          "legacy FPA read has 96-bit unavailable width");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 0,
              UVDB_RSP_REGISTER_LEGACY_FPS, &encoded_size) == 0 &&
              encoded_size == 8 && span_is(encoded, 8, 'x'),
          "legacy FPS read has 32-bit unavailable width");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_VFP_D_FIRST, &encoded_size) == 0 &&
              encoded_size == 16 &&
              memcmp(encoded, "efcdab8967452301", 16) == 0,
          "single D0 little-endian encoding");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_VFP_D_LAST, &encoded_size) == 0 &&
              memcmp(encoded, "1032547698badcfe", 16) == 0,
          "single D31 little-endian encoding");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_VFP_FPSCR, &encoded_size) == 0 &&
              encoded_size == 8 && memcmp(encoded, "00004000", 8) == 0,
          "single FPSCR little-endian encoding");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, NULL, 1,
              UVDB_RSP_REGISTER_VFP_D_FIRST, &encoded_size) == 0 &&
              encoded_size == 16 && span_is(encoded, 16, 'x'),
          "missing VFP snapshot encodes unavailable D register");

    char legacy_packet[UVDB_RSP_CORE_PACKET_HEX_SIZE];
    char vfp_packet[UVDB_RSP_VFP_PACKET_HEX_SIZE];
    size_t packet_size = 0;
    check(uvdb_rsp_encode_register_packet(
              legacy_packet, sizeof(legacy_packet), core, NULL, 0,
              &packet_size) == 0,
          "build legacy packet for single-register equivalence");
    for(uint32_t reg = UVDB_RSP_REGISTER_CORE_FIRST;
        reg <= UVDB_RSP_REGISTER_CORE_LAST; ++reg)
    {
        check(uvdb_rsp_encode_single_register(
                  encoded, sizeof(encoded), core, NULL, 0, reg,
                  &encoded_size) == 0 &&
                  !memcmp(encoded, legacy_packet + reg * 8u, 8u),
              "every legacy core p reply matches its g slot");
    }
    for(uint32_t reg = UVDB_RSP_REGISTER_LEGACY_FPA_FIRST;
        reg <= UVDB_RSP_REGISTER_LEGACY_FPA_LAST; ++reg)
    {
        const size_t offset = 16u * 8u +
            (reg - UVDB_RSP_REGISTER_LEGACY_FPA_FIRST) * 24u;
        check(uvdb_rsp_encode_single_register(
                  encoded, sizeof(encoded), core, NULL, 0, reg,
                  &encoded_size) == 0 && encoded_size == 24u &&
                  !memcmp(encoded, legacy_packet + offset, encoded_size),
              "every legacy FPA p reply matches its g slot");
    }
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, NULL, 0,
              UVDB_RSP_REGISTER_LEGACY_FPS, &encoded_size) == 0 &&
              !memcmp(encoded, legacy_packet + 16u * 8u + 8u * 24u, 8u),
          "legacy FPS p reply matches its g slot");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, NULL, 0,
              UVDB_RSP_REGISTER_CPSR, &encoded_size) == 0 &&
              !memcmp(encoded,
                      legacy_packet + 16u * 8u +
                          UVDB_RSP_LEGACY_FPA_BYTES * 2u,
                      8u),
          "legacy CPSR p reply matches its g slot");

    check(uvdb_rsp_encode_register_packet(
              vfp_packet, sizeof(vfp_packet), core, vfp, 1,
              &packet_size) == 0,
          "build VFP packet for single-register equivalence");
    for(uint32_t reg = UVDB_RSP_REGISTER_CORE_FIRST;
        reg <= UVDB_RSP_REGISTER_CORE_LAST; ++reg)
    {
        check(uvdb_rsp_encode_single_register(
                  encoded, sizeof(encoded), core, vfp, 1, reg,
                  &encoded_size) == 0 &&
                  !memcmp(encoded, vfp_packet + reg * 8u, 8u),
              "every VFP-layout core p reply matches its g slot");
    }
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_CPSR, &encoded_size) == 0 &&
              !memcmp(encoded, vfp_packet + 16u * 8u, 8u),
          "VFP-layout CPSR p reply matches its packed g slot");
    for(uint32_t reg = UVDB_RSP_REGISTER_VFP_D_FIRST;
        reg <= UVDB_RSP_REGISTER_VFP_D_LAST; ++reg)
    {
        const size_t offset = UVDB_RSP_EXPLICIT_CORE_PACKET_HEX_SIZE +
            (reg - UVDB_RSP_REGISTER_VFP_D_FIRST) * 16u;
        check(uvdb_rsp_encode_single_register(
                  encoded, sizeof(encoded), core, vfp, 1, reg,
                  &encoded_size) == 0 && encoded_size == 16u &&
                  !memcmp(encoded, vfp_packet + offset, encoded_size),
              "every D-register p reply matches its packed g slot");
    }
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1,
              UVDB_RSP_REGISTER_VFP_FPSCR, &encoded_size) == 0 &&
              !memcmp(encoded,
                      vfp_packet + UVDB_RSP_VFP_PACKET_HEX_SIZE - 8u, 8u),
          "FPSCR p reply matches its packed g slot");

    memset(encoded, '#', sizeof(encoded));
    check(uvdb_rsp_encode_single_register(
              encoded, 7, core, vfp, 0, 0, &encoded_size) ==
              UVDB_RSP_REGISTER_INVALID &&
              encoded_size == 8 && encoded[0] == '#',
          "short single-register buffer reports size without writing");
    encoded_size = SIZE_MAX;
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 1, 16,
              &encoded_size) == UVDB_RSP_REGISTER_INVALID &&
              encoded_size == 0,
          "single-register encoder honors VFP description gap");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), NULL, vfp, 0, 0,
              &encoded_size) == UVDB_RSP_REGISTER_INVALID,
          "single core read requires core snapshot");
    check(uvdb_rsp_encode_single_register(
              encoded, sizeof(encoded), core, vfp, 0, 0, NULL) ==
              UVDB_RSP_REGISTER_INVALID,
          "single read requires output size");
}

static void test_individual_core_writes(void)
{
    struct uvdb_rsp_core_register_write write = {
        .register_number = UINT32_C(0xaaaa5555),
        .value = UINT32_C(0xbbbb6666),
    };
    const struct uvdb_rsp_core_register_write sentinel = write;
    check(uvdb_rsp_parse_core_register_write_packet(
              "P0=78563412", 11, 0, &write) == 0 &&
              write.register_number == 0 &&
              write.value == UINT32_C(0x12345678),
          "parse little-endian r0 write");
    check(uvdb_rsp_parse_core_register_write_packet(
              "PF=EFCDAB89", 11, 1, &write) == 0 &&
              write.register_number == 15 &&
              write.value == UINT32_C(0x89abcdef),
          "parse uppercase PC write");
    check(uvdb_rsp_parse_core_register_write_packet(
              "P19=d4c3b2a1", 12, 1, &write) == 0 &&
              write.register_number == UVDB_RSP_REGISTER_CPSR &&
              write.value == UINT32_C(0xa1b2c3d4),
          "parse CPSR write");

    write = sentinel;
    check(uvdb_rsp_parse_core_register_write_packet(
              "P1a=efcdab8967452301", 20, 1, &write) ==
              UVDB_RSP_REGISTER_UNSUPPORTED &&
              !memcmp(&write, &sentinel, sizeof(write)),
          "D0 write is explicitly unsupported and non-mutating");
    check(uvdb_rsp_parse_core_register_write_packet(
              "P3a=00004000", 12, 1, &write) ==
              UVDB_RSP_REGISTER_UNSUPPORTED,
          "FPSCR write is explicitly unsupported");
    check(uvdb_rsp_parse_core_register_write_packet(
              "P10=000000000000000000000000", 28, 0, &write) ==
              UVDB_RSP_REGISTER_UNSUPPORTED,
          "legacy FPA write is explicitly unsupported");
    check(uvdb_rsp_parse_core_register_write_packet(
              "P18=00000000", 12, 0, &write) ==
              UVDB_RSP_REGISTER_UNSUPPORTED,
          "legacy FPS write is explicitly unsupported");

    const char* malformed[] = {
        "P", "P=00000000", "P0", "P0=", "P0=0000000",
        "P0=000000000", "P0=0000000x", "P0=00000000=", "P-1=00000000",
        "P100000000=00000000", "P10=00000000",
    };
    for(size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
    {
        write = sentinel;
        check(uvdb_rsp_parse_core_register_write_packet(
                  malformed[i], strlen(malformed[i]), 1, &write) ==
                  UVDB_RSP_REGISTER_INVALID &&
                  !memcmp(&write, &sentinel, sizeof(write)),
              "malformed core write rejected without output mutation");
    }
    check(uvdb_rsp_parse_core_register_write_packet(
              "P0=00000000", 11, 2, &write) ==
              UVDB_RSP_REGISTER_INVALID,
          "core write rejects unknown target layout");
    check(uvdb_rsp_parse_core_register_write_packet(
              NULL, 11, 0, &write) == UVDB_RSP_REGISTER_INVALID,
          "NULL write packet rejected");
    check(uvdb_rsp_parse_core_register_write_packet(
              "P0=00000000", 11, 0, NULL) ==
              UVDB_RSP_REGISTER_INVALID,
          "NULL parsed write rejected");

    struct uvdb_rsp_core_registers core = {0};
    for(unsigned int i = 0; i < UVDB_RSP_CORE_REGISTER_COUNT; ++i)
        core.r[i] = UINT32_C(0x10000000) + i;
    core.cpsr = UINT32_C(0x60000010);
    const struct uvdb_rsp_core_registers original = core;

    check(uvdb_rsp_parse_core_register_write_packet(
              "P7=78563412", 11, 1, &write) == 0,
          "parse mutation fixture");
    struct uvdb_rsp_core_register_write inverse = {
        .register_number = UINT32_MAX,
        .value = UINT32_MAX,
    };
    check(uvdb_rsp_apply_core_register_write(&core, &write, &inverse) == 0 &&
              core.r[7] == UINT32_C(0x12345678) &&
              inverse.register_number == 7 &&
              inverse.value == original.r[7],
          "core mutation captures exact inverse");
    check(uvdb_rsp_apply_core_register_write(&core, &inverse, NULL) == 0 &&
              !memcmp(&core, &original, sizeof(core)),
          "inverse restores exact core snapshot");

    check(uvdb_rsp_parse_core_register_write_packet(
              "P19=11000060", 12, 1, &write) == 0,
          "parse CPSR mutation fixture");
    check(uvdb_rsp_apply_core_register_write(&core, &write, &inverse) == 0 &&
              core.cpsr == UINT32_C(0x60000011) &&
              inverse.value == original.cpsr,
          "CPSR mutation captures exact inverse");
    check(uvdb_rsp_apply_core_register_write(&core, &inverse, NULL) == 0 &&
              !memcmp(&core, &original, sizeof(core)),
          "CPSR inverse restores exact snapshot");

    const struct uvdb_rsp_core_register_write invalid = {
        .register_number = UVDB_RSP_REGISTER_VFP_D_FIRST,
        .value = 1,
    };
    inverse.register_number = UINT32_MAX;
    inverse.value = UINT32_MAX;
    check(uvdb_rsp_apply_core_register_write(&core, &invalid, &inverse) ==
              UVDB_RSP_REGISTER_INVALID &&
              !memcmp(&core, &original, sizeof(core)) &&
              inverse.register_number == UINT32_MAX &&
              inverse.value == UINT32_MAX,
          "invalid mutation leaves state and inverse untouched");
    check(uvdb_rsp_apply_core_register_write(NULL, &write, &inverse) ==
              UVDB_RSP_REGISTER_INVALID,
          "NULL mutation target rejected");
    check(uvdb_rsp_apply_core_register_write(&core, NULL, &inverse) ==
              UVDB_RSP_REGISTER_INVALID,
          "NULL mutation request rejected");
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
    size = SIZE_MAX;
    check(uvdb_rsp_encode_register_packet(packet, sizeof(packet), &core,
                                           &vfp, 2, &size) < 0 && size == 0,
          "whole-register encoder rejects unknown target layout");

    test_individual_read_packets(&core, &vfp);
    test_individual_core_writes();

    if(failures)
        return 1;
    puts("PASS: ARM/VFP RSP register serialization");
    return 0;
}
