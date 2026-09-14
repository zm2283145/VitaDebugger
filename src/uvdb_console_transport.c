#include <string.h>

#include "uvdb_console_transport.h"
#include "uvdb_rsp.h"

static char transport_hex(unsigned int value)
{
    value &= 0xfu;
    return (char)(value < 10u ? '0' + value : 'a' + value - 10u);
}

static void transport_note_session_error(
    struct uvdb_console_transport* transport)
{
    transport->stats.session_errors++;
    transport->sent_uncommitted_sequence = 0;
    transport->failed = 1;
}

static int transport_ensure_session(
    struct uvdb_console_transport* transport)
{
    if(!transport->no_ack_mode)
        return UVDB_CONSOLE_EMPTY;
    if(transport->generation)
        return UVDB_CONSOLE_READY;

    uint32_t generation = uvdb_console_session_open();
    if(!generation)
        return UVDB_CONSOLE_BUSY;
    transport->generation = generation;
    return UVDB_CONSOLE_READY;
}

static size_t transport_frame_record(
    const struct uvdb_console_record* record,
    unsigned char frame[UVDB_CONSOLE_RSP_FRAME_MAX])
{
    size_t payload_size = 0;
    frame[0] = '$';
    if(uvdb_rsp_encode_console_payload(
            (char*)frame + 1,
            UVDB_CONSOLE_RSP_PAYLOAD_MAX,
            record->data,
            record->size,
            &payload_size) < 0)
        return 0;

    unsigned int checksum = 0;
    for(size_t index = 0; index < payload_size; ++index)
        checksum += frame[1 + index];
    frame[1 + payload_size] = '#';
    frame[2 + payload_size] = (unsigned char)transport_hex(checksum >> 4);
    frame[3 + payload_size] = (unsigned char)transport_hex(checksum);
    return payload_size + 4u;
}

void uvdb_console_transport_init(struct uvdb_console_transport* transport)
{
    if(transport)
        memset(transport, 0, sizeof(*transport));
}

int uvdb_console_transport_begin_connection(
    struct uvdb_console_transport* transport)
{
    if(!transport)
        return UVDB_CONSOLE_ERROR;
    int result = uvdb_console_transport_end_connection(transport);
    transport->no_ack_mode = 0;
    transport->sent_uncommitted_sequence = 0;
    transport->failed = 0;
    return result;
}

int uvdb_console_transport_enable_no_ack(
    struct uvdb_console_transport* transport)
{
    if(!transport)
        return UVDB_CONSOLE_ERROR;
    if(transport->failed)
        return UVDB_CONSOLE_ERROR;
    transport->no_ack_mode = 1;
    return transport_ensure_session(transport);
}

int uvdb_console_transport_end_connection(
    struct uvdb_console_transport* transport)
{
    if(!transport)
        return UVDB_CONSOLE_ERROR;

    uint32_t generation = transport->generation;
    transport->no_ack_mode = 0;
    transport->generation = 0;
    transport->sent_uncommitted_sequence = 0;
    transport->failed = 0;
    if(!generation)
        return UVDB_CONSOLE_READY;
    return uvdb_console_session_close(generation);
}

int uvdb_console_transport_no_ack(
    const struct uvdb_console_transport* transport)
{
    return transport && transport->no_ack_mode != 0;
}

int uvdb_console_transport_pump(
    struct uvdb_console_transport* transport,
    uvdb_console_write_fn write_fn,
    void* write_context)
{
    if(!transport || !write_fn)
        return UVDB_CONSOLE_PUMP_FATAL;
    if(transport->failed)
        return UVDB_CONSOLE_PUMP_FATAL;

    int session_result = transport_ensure_session(transport);
    if(session_result == UVDB_CONSOLE_EMPTY)
        return UVDB_CONSOLE_PUMP_IDLE;
    if(session_result == UVDB_CONSOLE_BUSY)
        return UVDB_CONSOLE_PUMP_BUSY;
    if(session_result != UVDB_CONSOLE_READY)
    {
        transport_note_session_error(transport);
        return UVDB_CONSOLE_PUMP_FATAL;
    }

    uint32_t generation = transport->generation;
    if(transport->sent_uncommitted_sequence)
    {
        int commit_result = uvdb_console_commit_sent(
            generation, transport->sent_uncommitted_sequence);
        if(commit_result == UVDB_CONSOLE_BUSY)
        {
            transport->stats.commit_busy++;
            return UVDB_CONSOLE_PUMP_BUSY;
        }
        if(commit_result != UVDB_CONSOLE_READY)
        {
            transport_note_session_error(transport);
            return UVDB_CONSOLE_PUMP_FATAL;
        }
        transport->sent_uncommitted_sequence = 0;
        return UVDB_CONSOLE_PUMP_COMMITTED;
    }

    struct uvdb_console_record record;
    int peek_result = uvdb_console_peek(generation, &record);
    if(peek_result == UVDB_CONSOLE_EMPTY)
        return UVDB_CONSOLE_PUMP_IDLE;
    if(peek_result == UVDB_CONSOLE_BUSY)
        return UVDB_CONSOLE_PUMP_BUSY;
    if(peek_result != UVDB_CONSOLE_READY)
    {
        transport_note_session_error(transport);
        return UVDB_CONSOLE_PUMP_FATAL;
    }

    unsigned char frame[UVDB_CONSOLE_RSP_FRAME_MAX];
    size_t frame_size = transport_frame_record(&record, frame);
    if(!frame_size)
    {
        transport_note_session_error(transport);
        return UVDB_CONSOLE_PUMP_FATAL;
    }

    size_t bytes_sent = 0;
    int native_error = 0;
    enum uvdb_console_write_result write_result = write_fn(
        write_context, frame, frame_size, &bytes_sent, &native_error);
    if(write_result == UVDB_CONSOLE_WRITE_WOULD_BLOCK && !bytes_sent)
    {
        transport->stats.would_block++;
        transport->stats.last_native_error = native_error;
        return UVDB_CONSOLE_PUMP_WOULD_BLOCK;
    }
    if(write_result != UVDB_CONSOLE_WRITE_COMPLETE)
    {
        if(bytes_sent)
            transport->stats.partial_writes++;
        else
            transport->stats.hard_errors++;
        transport->stats.last_native_error = native_error;
        transport->failed = 1;
        return UVDB_CONSOLE_PUMP_FATAL;
    }
    if(bytes_sent != frame_size)
    {
        transport->stats.partial_writes++;
        transport->stats.last_native_error = native_error;
        transport->failed = 1;
        return UVDB_CONSOLE_PUMP_FATAL;
    }

    /* A test callback may deliberately rotate the session during the write.
     * Never apply an old commit token to the new connection in that case. */
    if(!transport->no_ack_mode || transport->generation != generation)
    {
        transport_note_session_error(transport);
        return UVDB_CONSOLE_PUMP_FATAL;
    }

    transport->stats.frames_sent++;
    transport->stats.frame_bytes_sent += (uint32_t)frame_size;
    transport->sent_uncommitted_sequence = record.sequence;
    int commit_result = uvdb_console_commit_sent(generation, record.sequence);
    if(commit_result == UVDB_CONSOLE_BUSY)
    {
        transport->stats.commit_busy++;
        return UVDB_CONSOLE_PUMP_SENT;
    }
    if(commit_result != UVDB_CONSOLE_READY)
    {
        transport_note_session_error(transport);
        return UVDB_CONSOLE_PUMP_FATAL;
    }
    transport->sent_uncommitted_sequence = 0;
    return UVDB_CONSOLE_PUMP_SENT;
}

int uvdb_console_transport_get_stats(
    const struct uvdb_console_transport* transport,
    struct uvdb_console_transport_stats* stats)
{
    if(!transport || !stats)
        return -1;
    *stats = transport->stats;
    return 0;
}
