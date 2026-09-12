#include "uvdb_rsp.h"

static char hex_digit(unsigned int value)
{
    return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
}

static void write_byte(char** output, uint8_t value)
{
    *(*output)++ = hex_digit(value >> 4);
    *(*output)++ = hex_digit(value & 0xf);
}

static void write_u32_le(char** output, uint32_t value)
{
    for(unsigned int i = 0; i < 4; ++i)
        write_byte(output, (uint8_t)(value >> (i * 8)));
}

static void write_u64_le(char** output, uint64_t value)
{
    for(unsigned int i = 0; i < 8; ++i)
        write_byte(output, (uint8_t)(value >> (i * 8)));
}

static void write_unavailable(char** output, size_t byte_count)
{
    for(size_t i = 0; i < byte_count; ++i)
    {
        *(*output)++ = 'x';
        *(*output)++ = 'x';
    }
}

int uvdb_rsp_encode_register_packet(
    char* output,
    size_t capacity,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp,
    int include_vfp,
    size_t* output_size)
{
    size_t required = include_vfp ? UVDB_RSP_VFP_PACKET_HEX_SIZE
                                  : UVDB_RSP_CORE_PACKET_HEX_SIZE;
    if(!output_size)
        return -1;
    *output_size = required;
    if(!output || !core || capacity < required)
        return -1;

    char* cursor = output;
    for(unsigned int i = 0; i < UVDB_RSP_CORE_REGISTER_COUNT; ++i)
        write_u32_le(&cursor, core->r[i]);

    if(include_vfp)
    {
        // With an explicit target description, GDB packs only described
        // registers; the regnum 16-24 gap is not represented in `g`.
        write_u32_le(&cursor, core->cpsr);
        if(vfp)
        {
            for(unsigned int i = 0; i < UVDB_RSP_VFP_D_REGISTER_COUNT; ++i)
                write_u64_le(&cursor, vfp->d[i]);
            write_u32_le(&cursor, vfp->fpscr);
        }
        else
        {
            write_unavailable(&cursor, UVDB_RSP_VFP_PACKET_BYTES);
        }
    }
    else
    {
        write_unavailable(&cursor, UVDB_RSP_LEGACY_FPA_BYTES);
        write_u32_le(&cursor, core->cpsr);
    }

    return cursor == output + required ? 0 : -1;
}
