#include "uvdb_rsp.h"

static char hex_digit(unsigned int value)
{
    return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
}

static int hex_value(char digit, uint8_t* value)
{
    if(!value)
        return -1;
    if(digit >= '0' && digit <= '9')
        *value = (uint8_t)(digit - '0');
    else if(digit >= 'a' && digit <= 'f')
        *value = (uint8_t)(digit - 'a' + 10);
    else if(digit >= 'A' && digit <= 'F')
        *value = (uint8_t)(digit - 'A' + 10);
    else
        return -1;
    return 0;
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

enum register_kind {
    REGISTER_KIND_CORE,
    REGISTER_KIND_CPSR,
    REGISTER_KIND_LEGACY_FPA,
    REGISTER_KIND_LEGACY_FPS,
    REGISTER_KIND_VFP_D,
    REGISTER_KIND_VFP_FPSCR,
};

struct register_description {
    enum register_kind kind;
    uint32_t index;
    size_t byte_size;
};

static int valid_layout(int include_vfp)
{
    return include_vfp == 0 || include_vfp == 1;
}

static int describe_register(
    uint32_t register_number,
    int include_vfp,
    struct register_description* description)
{
    if(!description || !valid_layout(include_vfp))
        return UVDB_RSP_REGISTER_INVALID;

    struct register_description candidate;
    if(register_number <= UVDB_RSP_REGISTER_CORE_LAST)
    {
        candidate.kind = REGISTER_KIND_CORE;
        candidate.index = register_number;
        candidate.byte_size = sizeof(uint32_t);
    }
    else if(register_number == UVDB_RSP_REGISTER_CPSR)
    {
        candidate.kind = REGISTER_KIND_CPSR;
        candidate.index = 0;
        candidate.byte_size = sizeof(uint32_t);
    }
    else if(include_vfp &&
            register_number >= UVDB_RSP_REGISTER_VFP_D_FIRST &&
            register_number <= UVDB_RSP_REGISTER_VFP_D_LAST)
    {
        candidate.kind = REGISTER_KIND_VFP_D;
        candidate.index = register_number - UVDB_RSP_REGISTER_VFP_D_FIRST;
        candidate.byte_size = sizeof(uint64_t);
    }
    else if(include_vfp &&
            register_number == UVDB_RSP_REGISTER_VFP_FPSCR)
    {
        candidate.kind = REGISTER_KIND_VFP_FPSCR;
        candidate.index = 0;
        candidate.byte_size = sizeof(uint32_t);
    }
    else if(!include_vfp &&
            register_number >= UVDB_RSP_REGISTER_LEGACY_FPA_FIRST &&
            register_number <= UVDB_RSP_REGISTER_LEGACY_FPA_LAST)
    {
        candidate.kind = REGISTER_KIND_LEGACY_FPA;
        candidate.index = register_number -
                          UVDB_RSP_REGISTER_LEGACY_FPA_FIRST;
        candidate.byte_size = 12u;
    }
    else if(!include_vfp &&
            register_number == UVDB_RSP_REGISTER_LEGACY_FPS)
    {
        candidate.kind = REGISTER_KIND_LEGACY_FPS;
        candidate.index = 0;
        candidate.byte_size = sizeof(uint32_t);
    }
    else
        return UVDB_RSP_REGISTER_INVALID;

    *description = candidate;
    return UVDB_RSP_REGISTER_OK;
}

static int parse_register_number(
    const char* text,
    size_t size,
    uint32_t* register_number)
{
    if(!text || !register_number || !size || size > 8u)
        return UVDB_RSP_REGISTER_INVALID;

    uint32_t value = 0;
    for(size_t i = 0; i < size; ++i)
    {
        uint8_t nibble;
        if(hex_value(text[i], &nibble) < 0)
            return UVDB_RSP_REGISTER_INVALID;
        if(value > (UINT32_MAX - nibble) / 16u)
            return UVDB_RSP_REGISTER_INVALID;
        value = value * 16u + nibble;
    }
    *register_number = value;
    return UVDB_RSP_REGISTER_OK;
}

static int parse_u32_hex(const char* text, size_t size, uint32_t* output)
{
    if(!text || !output || !size || size > 8u)
        return -1;
    uint32_t value = 0;
    for(size_t i = 0; i < size; ++i)
    {
        uint8_t nibble;
        if(hex_value(text[i], &nibble) < 0 ||
           value > (UINT32_MAX - nibble) / 16u)
            return -1;
        value = value * 16u + nibble;
    }
    *output = value;
    return 0;
}

static int parse_u64_hex(const char* text, size_t size, uint64_t* output)
{
    if(!text || !output || !size || size > 16u)
        return -1;
    uint64_t value = 0;
    for(size_t i = 0; i < size; ++i)
    {
        uint8_t nibble;
        if(hex_value(text[i], &nibble) < 0 ||
           value > (UINT64_MAX - nibble) / UINT64_C(16))
            return -1;
        value = value * UINT64_C(16) + nibble;
    }
    *output = value;
    return 0;
}

static int validate_hex_value(
    const char* text,
    size_t size,
    size_t expected_bytes)
{
    if(!text || expected_bytes > SIZE_MAX / 2u ||
       size != expected_bytes * 2u)
        return UVDB_RSP_REGISTER_INVALID;
    for(size_t i = 0; i < size; ++i)
    {
        uint8_t unused;
        if(hex_value(text[i], &unused) < 0)
            return UVDB_RSP_REGISTER_INVALID;
    }
    return UVDB_RSP_REGISTER_OK;
}

static int read_u32_le_hex(const char* text, uint32_t* value)
{
    if(!text || !value)
        return UVDB_RSP_REGISTER_INVALID;
    uint32_t decoded = 0;
    for(unsigned int byte_index = 0; byte_index < 4u; ++byte_index)
    {
        uint8_t high = 0;
        uint8_t low = 0;
        if(hex_value(text[byte_index * 2u], &high) < 0 ||
           hex_value(text[byte_index * 2u + 1u], &low) < 0)
            return UVDB_RSP_REGISTER_INVALID;
        decoded |= (uint32_t)((high << 4) | low) << (byte_index * 8u);
    }
    *value = decoded;
    return UVDB_RSP_REGISTER_OK;
}

static int spans_overlap(const void* first, size_t first_size,
                         const void* second, size_t second_size)
{
    if(!first_size || !second_size)
        return 0;
    uintptr_t first_address = (uintptr_t)first;
    uintptr_t second_address = (uintptr_t)second;
    if(first_address <= second_address)
        return second_address - first_address < first_size;
    return first_address - second_address < second_size;
}

int uvdb_rsp_encode_console_payload(
    char* output,
    size_t capacity,
    const void* data,
    size_t data_size,
    size_t* output_size)
{
    if(!output_size)
        return -1;
    if(data_size > (SIZE_MAX - 1u) / 2u)
    {
        *output_size = SIZE_MAX;
        return -1;
    }

    size_t required = 1u + data_size * 2u;
    *output_size = required;
    if(!output || (!data && data_size) || capacity < required)
        return -1;
    if(spans_overlap(output, required, data, data_size))
        return -1;

    const uint8_t* bytes = data;
    char* cursor = output;
    *cursor++ = 'O';
    for(size_t i = 0; i < data_size; ++i)
        write_byte(&cursor, bytes[i]);
    return cursor == output + required ? 0 : -1;
}

int uvdb_rsp_encode_register_packet(
    char* output,
    size_t capacity,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp,
    int include_vfp,
    size_t* output_size)
{
    if(!output_size)
        return -1;
    if(!valid_layout(include_vfp))
    {
        *output_size = 0;
        return -1;
    }
    size_t required = include_vfp ? UVDB_RSP_VFP_PACKET_HEX_SIZE
                                  : UVDB_RSP_CORE_PACKET_HEX_SIZE;
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

int uvdb_rsp_parse_register_read_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    uint32_t* register_number)
{
    if(!packet || !register_number || packet_size < 2u ||
       packet[0] != 'p' || !valid_layout(include_vfp))
        return UVDB_RSP_REGISTER_INVALID;

    uint32_t parsed;
    struct register_description description;
    if(parse_register_number(packet + 1u, packet_size - 1u, &parsed) < 0 ||
       describe_register(parsed, include_vfp, &description) < 0)
        return UVDB_RSP_REGISTER_INVALID;
    *register_number = parsed;
    return UVDB_RSP_REGISTER_OK;
}

int uvdb_rsp_encode_single_register(
    char* output,
    size_t capacity,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp,
    int include_vfp,
    uint32_t register_number,
    size_t* output_size)
{
    if(!output_size)
        return UVDB_RSP_REGISTER_INVALID;
    *output_size = 0;

    struct register_description description;
    if(describe_register(register_number, include_vfp, &description) < 0 ||
       description.byte_size > SIZE_MAX / 2u)
        return UVDB_RSP_REGISTER_INVALID;

    const size_t required = description.byte_size * 2u;
    *output_size = required;
    if(!output || capacity < required)
        return UVDB_RSP_REGISTER_INVALID;

    char* cursor = output;
    switch(description.kind)
    {
        case REGISTER_KIND_CORE:
            if(!core)
                return UVDB_RSP_REGISTER_INVALID;
            write_u32_le(&cursor, core->r[description.index]);
            break;
        case REGISTER_KIND_CPSR:
            if(!core)
                return UVDB_RSP_REGISTER_INVALID;
            write_u32_le(&cursor, core->cpsr);
            break;
        case REGISTER_KIND_LEGACY_FPA:
        case REGISTER_KIND_LEGACY_FPS:
            write_unavailable(&cursor, description.byte_size);
            break;
        case REGISTER_KIND_VFP_D:
            if(vfp)
                write_u64_le(&cursor, vfp->d[description.index]);
            else
                write_unavailable(&cursor, description.byte_size);
            break;
        case REGISTER_KIND_VFP_FPSCR:
            if(vfp)
                write_u32_le(&cursor, vfp->fpscr);
            else
                write_unavailable(&cursor, description.byte_size);
            break;
    }
    return cursor == output + required ? UVDB_RSP_REGISTER_OK
                                        : UVDB_RSP_REGISTER_INVALID;
}

int uvdb_rsp_parse_core_register_write_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    struct uvdb_rsp_core_register_write* write)
{
    if(!packet || !write || packet_size < 4u || packet[0] != 'P' ||
       !valid_layout(include_vfp))
        return UVDB_RSP_REGISTER_INVALID;

    size_t equals = 1u;
    while(equals < packet_size && packet[equals] != '=')
        equals++;
    if(equals == 1u || equals == packet_size)
        return UVDB_RSP_REGISTER_INVALID;

    uint32_t register_number;
    struct register_description description;
    if(parse_register_number(packet + 1u, equals - 1u,
                             &register_number) < 0 ||
       describe_register(register_number, include_vfp, &description) < 0)
        return UVDB_RSP_REGISTER_INVALID;

    const char* value_text = packet + equals + 1u;
    const size_t value_size = packet_size - equals - 1u;
    if(validate_hex_value(value_text, value_size,
                          description.byte_size) < 0)
        return UVDB_RSP_REGISTER_INVALID;

    if(description.kind != REGISTER_KIND_CORE &&
       description.kind != REGISTER_KIND_CPSR)
        return UVDB_RSP_REGISTER_UNSUPPORTED;

    struct uvdb_rsp_core_register_write parsed = {
        .register_number = register_number,
    };
    if(read_u32_le_hex(value_text, &parsed.value) < 0)
        return UVDB_RSP_REGISTER_INVALID;
    *write = parsed;
    return UVDB_RSP_REGISTER_OK;
}

int uvdb_rsp_apply_core_register_write(
    struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_core_register_write* write,
    struct uvdb_rsp_core_register_write* inverse)
{
    if(!core || !write)
        return UVDB_RSP_REGISTER_INVALID;

    uint32_t previous;
    if(write->register_number <= UVDB_RSP_REGISTER_CORE_LAST)
        previous = core->r[write->register_number];
    else if(write->register_number == UVDB_RSP_REGISTER_CPSR)
        previous = core->cpsr;
    else
        return UVDB_RSP_REGISTER_INVALID;

    struct uvdb_rsp_core_register_write undo = {
        .register_number = write->register_number,
        .value = previous,
    };
    if(write->register_number <= UVDB_RSP_REGISTER_CORE_LAST)
        core->r[write->register_number] = write->value;
    else
        core->cpsr = write->value;
    if(inverse)
        *inverse = undo;
    return UVDB_RSP_REGISTER_OK;
}

static int validate_unavailable_or_hex_bytes(
    const char* text,
    size_t byte_count)
{
    if(!text || byte_count > SIZE_MAX / 2u)
        return -1;
    for(size_t i = 0; i < byte_count; ++i)
    {
        const char first = text[i * 2u];
        const char second = text[i * 2u + 1u];
        if((first == 'x' || first == 'X') &&
           (second == 'x' || second == 'X'))
            continue;
        uint8_t unused;
        if(hex_value(first, &unused) < 0 ||
           hex_value(second, &unused) < 0)
            return -1;
    }
    return 0;
}

int uvdb_rsp_decode_hex_bytes(
    void* output,
    size_t output_size,
    const char* text,
    size_t text_size)
{
    if((output_size && (!output || !text)) ||
       output_size > SIZE_MAX / 2u || text_size != output_size * 2u)
        return -1;
    if(!output_size)
        return text_size == 0 ? 0 : -1;
    if(spans_overlap(output, output_size, text, text_size))
        return -1;

    /* Validate first so a bad final nibble cannot leave a partial mutation. */
    if(validate_hex_value(text, text_size, output_size) < 0)
        return -1;

    uint8_t* bytes = output;
    for(size_t i = 0; i < output_size; ++i)
    {
        uint8_t high = 0;
        uint8_t low = 0;
        (void)hex_value(text[i * 2u], &high);
        (void)hex_value(text[i * 2u + 1u], &low);
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

int uvdb_rsp_parse_core_register_packet(
    const char* packet,
    size_t packet_size,
    int include_vfp,
    struct uvdb_rsp_core_registers* core)
{
    if(!packet || !core || !valid_layout(include_vfp) || packet_size !=
           1u + UVDB_RSP_CORE_PACKET_HEX_SIZE || packet[0] != 'G')
        return UVDB_RSP_REGISTER_INVALID;
    if(include_vfp)
        return UVDB_RSP_REGISTER_UNSUPPORTED;
    if(include_vfp != 0)
        return UVDB_RSP_REGISTER_INVALID;

    const char* values = packet + 1u;
    const size_t core_hex_size =
        UVDB_RSP_CORE_REGISTER_COUNT * sizeof(uint32_t) * 2u;
    const size_t unavailable_hex_size = UVDB_RSP_LEGACY_FPA_BYTES * 2u;
    if(validate_hex_value(values, core_hex_size,
                          UVDB_RSP_CORE_REGISTER_COUNT *
                              sizeof(uint32_t)) < 0 ||
       validate_unavailable_or_hex_bytes(values + core_hex_size,
                                          UVDB_RSP_LEGACY_FPA_BYTES) < 0 ||
       validate_hex_value(values + core_hex_size + unavailable_hex_size,
                          sizeof(uint32_t) * 2u,
                          sizeof(uint32_t)) < 0)
        return UVDB_RSP_REGISTER_INVALID;

    struct uvdb_rsp_core_registers candidate;
    for(size_t i = 0; i < UVDB_RSP_CORE_REGISTER_COUNT; ++i)
        if(read_u32_le_hex(values + i * sizeof(uint32_t) * 2u,
                           &candidate.r[i]) < 0)
            return UVDB_RSP_REGISTER_INVALID;
    if(read_u32_le_hex(values + core_hex_size + unavailable_hex_size,
                       &candidate.cpsr) < 0)
        return UVDB_RSP_REGISTER_INVALID;
    *core = candidate;
    return UVDB_RSP_REGISTER_OK;
}

static int parse_memory_packet(
    const char* packet,
    size_t packet_size,
    size_t maximum_size,
    int write,
    struct uvdb_rsp_memory_request* request)
{
    if(!packet || !request || packet_size < (write ? 5u : 4u) ||
       packet[0] != (write ? 'M' : 'm'))
        return -1;

    size_t comma = 1u;
    while(comma < packet_size && packet[comma] != ',')
        ++comma;
    if(comma == 1u || comma == packet_size)
        return -1;

    const char terminator = write ? ':' : '\0';
    size_t end_length = comma + 1u;
    while(end_length < packet_size &&
          (!write || packet[end_length] != terminator))
        ++end_length;
    if(end_length == comma + 1u ||
       (write && end_length == packet_size) ||
       (!write && end_length != packet_size))
        return -1;

    uint32_t address;
    uint32_t parsed_size;
    if(parse_u32_hex(packet + 1u, comma - 1u, &address) < 0 ||
       parse_u32_hex(packet + comma + 1u,
                     end_length - comma - 1u, &parsed_size) < 0 ||
       (size_t)parsed_size > maximum_size ||
       (uint64_t)address + (uint64_t)parsed_size >
           (UINT64_C(1) << 32))
        return -1;

    const char* data = NULL;
    if(write)
    {
        const size_t data_offset = end_length + 1u;
        if(parsed_size > UINT32_MAX / 2u ||
           packet_size - data_offset != (size_t)parsed_size * 2u ||
           validate_hex_value(packet + data_offset,
                              packet_size - data_offset,
                              parsed_size) < 0)
            return -1;
        data = packet + data_offset;
    }

    const struct uvdb_rsp_memory_request candidate = {
        .address = address,
        .size = parsed_size,
        .data = data,
    };
    *request = candidate;
    return 0;
}

int uvdb_rsp_parse_memory_read_packet(
    const char* packet,
    size_t packet_size,
    size_t maximum_size,
    struct uvdb_rsp_memory_request* request)
{
    return parse_memory_packet(packet, packet_size, maximum_size, 0,
                               request);
}

int uvdb_rsp_parse_memory_write_packet(
    const char* packet,
    size_t packet_size,
    size_t maximum_size,
    struct uvdb_rsp_memory_request* request)
{
    return parse_memory_packet(packet, packet_size, maximum_size, 1,
                               request);
}

int uvdb_rsp_parse_software_breakpoint_packet(
    const char* packet,
    size_t packet_size,
    int* insert,
    uint32_t* address,
    size_t* kind)
{
    if(!packet || !insert || !address || !kind || packet_size < 6u ||
       (packet[0] != 'Z' && packet[0] != 'z') ||
       packet[1] != '0' || packet[2] != ',')
        return -1;
    size_t comma = 3u;
    while(comma < packet_size && packet[comma] != ',')
        ++comma;
    if(comma == 3u || comma == packet_size || comma + 1u == packet_size)
        return -1;

    uint32_t parsed_address;
    uint32_t parsed_kind;
    if(parse_u32_hex(packet + 3u, comma - 3u, &parsed_address) < 0 ||
       parse_u32_hex(packet + comma + 1u,
                     packet_size - comma - 1u, &parsed_kind) < 0)
        return -1;
    *insert = packet[0] == 'Z';
    *address = parsed_address;
    *kind = (size_t)parsed_kind;
    return 0;
}

int uvdb_rsp_parse_xfer_range(
    const char* text,
    size_t text_size,
    struct uvdb_rsp_xfer_range* range)
{
    if(!text || !range || text_size < 3u)
        return -1;
    size_t comma = 0;
    while(comma < text_size && text[comma] != ',')
        ++comma;
    if(!comma || comma == text_size || comma + 1u == text_size)
        return -1;
    uint64_t offset;
    uint64_t length;
    if(parse_u64_hex(text, comma, &offset) < 0 ||
       parse_u64_hex(text + comma + 1u,
                     text_size - comma - 1u, &length) < 0 ||
       offset > UINT64_MAX - length)
        return -1;
    const struct uvdb_rsp_xfer_range candidate = {
        .offset = offset,
        .length = length,
    };
    *range = candidate;
    return 0;
}

int uvdb_rsp_parse_fileio_packet(
    const char* packet,
    size_t packet_size,
    struct uvdb_rsp_fileio_result* result)
{
    if(!packet || !result || packet_size < 2u || packet[0] != 'F')
        return -1;

    size_t header_end = 1u;
    while(header_end < packet_size && packet[header_end] != ';')
        ++header_end;
    const int has_attachment = header_end < packet_size;

    size_t cursor = 1u;
    int negative = 0;
    if(packet[cursor] == '-')
    {
        negative = 1;
        if(++cursor == header_end)
            return -1;
    }
    size_t end = cursor;
    while(end < header_end && packet[end] != ',')
        ++end;
    uint32_t magnitude;
    if(parse_u32_hex(packet + cursor, end - cursor, &magnitude) < 0 ||
       (negative && magnitude > UINT32_C(0x80000000)))
        return -1;

    struct uvdb_rsp_fileio_result candidate = {
        .result = negative ? 0u - magnitude : magnitude,
        .has_attachment = has_attachment,
        .attachment = has_attachment ? packet + header_end + 1u : NULL,
        .attachment_size = has_attachment
                               ? packet_size - header_end - 1u : 0u,
    };

    if(end < header_end)
    {
        cursor = end + 1u;
        if(cursor == header_end)
            return -1;
        end = cursor;
        while(end < header_end && packet[end] != ',')
            ++end;
        if(parse_u32_hex(packet + cursor, end - cursor,
                         &candidate.error_number) < 0)
            return -1;
        candidate.has_error_number = 1;

        if(end < header_end)
        {
            cursor = end + 1u;
            if(header_end - cursor != 1u || packet[cursor] != 'C')
                return -1;
            candidate.interrupted = 1;
            end = header_end;
        }
    }
    if(end != header_end)
        return -1;
    *result = candidate;
    return 0;
}

int uvdb_rsp_parse_fileio_result_packet(
    const char* packet,
    size_t packet_size,
    uint32_t* result)
{
    if(!result)
        return -1;
    struct uvdb_rsp_fileio_result parsed;
    if(uvdb_rsp_parse_fileio_packet(packet, packet_size, &parsed) < 0)
        return -1;
    *result = parsed.result;
    return 0;
}
