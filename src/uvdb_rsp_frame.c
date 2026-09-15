#include "uvdb_rsp_frame.h"

#include <stdint.h>
#include <string.h>

static int hex_value(unsigned char digit)
{
    if(digit >= '0' && digit <= '9')
        return digit - '0';
    if(digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if(digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

int uvdb_rsp_scan_frame(
    const void* input,
    size_t input_size,
    size_t maximum_payload_size,
    struct uvdb_rsp_frame* frame)
{
    if((input_size && !input) || !frame)
        return UVDB_RSP_FRAME_DISCARD;

    const unsigned char* bytes = input;
    struct uvdb_rsp_frame result = {0};
    if(!input_size)
    {
        *frame = result;
        return UVDB_RSP_FRAME_INCOMPLETE;
    }

    size_t start = 0;
    while(start < input_size && bytes[start] != '$')
        ++start;
    if(start)
    {
        result.consumed_size = start;
        *frame = result;
        return UVDB_RSP_FRAME_DISCARD;
    }

    size_t marker = 1u;
    while(marker < input_size && bytes[marker] != '#')
    {
        /* A new start marker unambiguously supersedes an incomplete frame. */
        if(bytes[marker] == '$')
        {
            result.consumed_size = marker;
            result.request_nack = 1u;
            *frame = result;
            return UVDB_RSP_FRAME_DISCARD;
        }
        if(marker - 1u >= maximum_payload_size)
        {
            result.consumed_size = marker + 1u;
            result.request_nack = 1u;
            *frame = result;
            return UVDB_RSP_FRAME_DISCARD;
        }
        ++marker;
    }

    if(marker == input_size)
    {
        if(input_size - 1u > maximum_payload_size)
        {
            result.consumed_size = input_size;
            result.request_nack = 1u;
            *frame = result;
            return UVDB_RSP_FRAME_DISCARD;
        }
        *frame = result;
        return UVDB_RSP_FRAME_INCOMPLETE;
    }
    if(marker - 1u > maximum_payload_size)
    {
        result.consumed_size = marker + 1u;
        result.request_nack = 1u;
        *frame = result;
        return UVDB_RSP_FRAME_DISCARD;
    }
    if(input_size - marker < 3u)
    {
        *frame = result;
        return UVDB_RSP_FRAME_INCOMPLETE;
    }

    const int high = hex_value(bytes[marker + 1u]);
    const int low = hex_value(bytes[marker + 2u]);
    result.consumed_size = marker + 3u;
    if(high < 0 || low < 0)
    {
        result.request_nack = 1u;
        *frame = result;
        return UVDB_RSP_FRAME_DISCARD;
    }

    uint8_t checksum = 0;
    for(size_t i = 1u; i < marker; ++i)
        checksum = (uint8_t)(checksum + bytes[i]);
    if(checksum != (uint8_t)((high << 4) | low))
    {
        result.request_nack = 1u;
        *frame = result;
        return UVDB_RSP_FRAME_DISCARD;
    }

    result.payload_offset = 1u;
    result.payload_size = marker - 1u;
    *frame = result;
    return UVDB_RSP_FRAME_COMPLETE;
}

int uvdb_rsp_frame_should_nack(
    const struct uvdb_rsp_frame* frame,
    int no_ack_mode)
{
    if(!frame || (no_ack_mode != 0 && no_ack_mode != 1))
        return 0;
    return !no_ack_mode && frame->request_nack == 1u;
}

void uvdb_rsp_request_lifetime_init(
    struct uvdb_rsp_request_lifetime* lifetime)
{
    if(lifetime)
        memset(lifetime, 0, sizeof(*lifetime));
}

int uvdb_rsp_request_lifetime_begin(
    struct uvdb_rsp_request_lifetime* lifetime)
{
    if(!lifetime || lifetime->active)
        return -1;
    lifetime->active = 1u;
    return 0;
}

int uvdb_rsp_request_lifetime_release(
    struct uvdb_rsp_request_lifetime* lifetime)
{
    if(!lifetime || !lifetime->active)
        return -1;
    lifetime->active = 0u;
    return 0;
}

int uvdb_rsp_request_lifetime_is_active(
    const struct uvdb_rsp_request_lifetime* lifetime)
{
    return lifetime && lifetime->active == 1u;
}
