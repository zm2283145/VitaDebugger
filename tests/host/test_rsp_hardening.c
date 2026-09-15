#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_fileio_flow.h"
#include "uvdb_rsp.h"
#include "uvdb_rsp_frame.h"
#include "uvdb_thread_control.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static uint32_t random_state = UINT32_C(0x6d2b79f5);

static uint32_t next_random(void)
{
    uint32_t value = random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    random_state = value;
    return value;
}

static size_t count_discard_nacks(
    const char* input,
    size_t input_size,
    size_t maximum_payload_size,
    int no_ack_mode)
{
    size_t count = 0;
    while(input_size)
    {
        struct uvdb_rsp_frame frame = {0, 0, 0, 0};
        int result = uvdb_rsp_scan_frame(
            input, input_size, maximum_payload_size, &frame);
        if(result != UVDB_RSP_FRAME_DISCARD)
            break;
        if(uvdb_rsp_frame_should_nack(&frame, no_ack_mode))
            ++count;
        if(!frame.consumed_size || frame.consumed_size > input_size)
            return SIZE_MAX;
        input += frame.consumed_size;
        input_size -= frame.consumed_size;
    }
    return count;
}

static void test_frames(void)
{
    struct uvdb_rsp_frame frame = {99, 99, 99, 99};
    check(uvdb_rsp_scan_frame("$m0,1#fa", strlen("$m0,1#fa"), 1024,
                              &frame) ==
              UVDB_RSP_FRAME_COMPLETE &&
          frame.payload_offset == 1 && frame.payload_size == 4 &&
          frame.consumed_size == strlen("$m0,1#fa"),
          "accept exact lowercase-checksum frame");
    check(uvdb_rsp_scan_frame("$m0,1#FA", strlen("$m0,1#FA"), 1024,
                              &frame) ==
              UVDB_RSP_FRAME_COMPLETE,
          "accept uppercase checksum digits");
    check(uvdb_rsp_scan_frame("noise$m0,1#fa", strlen("noise$m0,1#fa"),
                              1024, &frame) ==
              UVDB_RSP_FRAME_DISCARD && frame.consumed_size == 5,
          "discard bounded prefix before frame");
    check(uvdb_rsp_scan_frame("$bad$m0,1#fa", strlen("$bad$m0,1#fa"),
                              1024, &frame) ==
              UVDB_RSP_FRAME_DISCARD && frame.consumed_size == 4,
          "resynchronize at newer start marker");
    check(uvdb_rsp_scan_frame("$m0,1#f", strlen("$m0,1#f"), 1024,
                              &frame) ==
              UVDB_RSP_FRAME_INCOMPLETE && frame.consumed_size == 0,
          "retain truncated checksum");
    check(uvdb_rsp_scan_frame("$m0,1#00", strlen("$m0,1#00"), 1024,
                              &frame) == UVDB_RSP_FRAME_DISCARD &&
          frame.consumed_size == strlen("$m0,1#00") &&
          uvdb_rsp_frame_should_nack(&frame, 0) &&
          !uvdb_rsp_frame_should_nack(&frame, 1),
          "discard checksum mismatch");
    check(uvdb_rsp_scan_frame("$m0,1#zz", strlen("$m0,1#zz"), 1024,
                              &frame) == UVDB_RSP_FRAME_DISCARD &&
          frame.consumed_size == strlen("$m0,1#zz"),
          "discard nonhex checksum");
    check(uvdb_rsp_scan_frame("$12345", 6, 4, &frame) ==
              UVDB_RSP_FRAME_DISCARD && frame.consumed_size != 0 &&
          uvdb_rsp_frame_should_nack(&frame, 0),
          "discard oversized unterminated payload");

    static const char corrupt_then_valid[] = "$m0,1#00$m0,1#fa";
    check(count_discard_nacks(
              corrupt_then_valid, sizeof(corrupt_then_valid) - 1u,
              1024u, 0) == 1u,
          "ACK mode requests exactly one NACK for one malformed frame");
    check(count_discard_nacks(
              corrupt_then_valid, sizeof(corrupt_then_valid) - 1u,
              1024u, 1) == 0u,
          "no-ack mode suppresses malformed-frame NACK");
    check(count_discard_nacks(
              "noise$m0,1#fa", strlen("noise$m0,1#fa"), 1024u,
              0) == 0u,
          "prefix resynchronization does not emit a spurious NACK");

    struct uvdb_rsp_request_lifetime lifetime = {99};
    uvdb_rsp_request_lifetime_init(&lifetime);
    check(!uvdb_rsp_request_lifetime_is_active(&lifetime) &&
              uvdb_rsp_request_lifetime_begin(&lifetime) == 0 &&
              uvdb_rsp_request_lifetime_is_active(&lifetime),
          "completed request borrows the shared receive buffer");
    check(uvdb_rsp_request_lifetime_begin(&lifetime) < 0,
          "a second packet cannot reuse a borrowed receive buffer");
    check(uvdb_rsp_request_lifetime_release(&lifetime) == 0 &&
              !uvdb_rsp_request_lifetime_is_active(&lifetime),
          "discard releases the request before response ACK collection");
    check(uvdb_rsp_request_lifetime_release(&lifetime) < 0,
          "double release is rejected");
}

static void test_memory_packets(void)
{
    struct uvdb_rsp_memory_request request = {
        UINT32_C(0xaaaaaaaa), 0xbbbb, (const char*)(uintptr_t)0xcccc,
    };
    check(uvdb_rsp_parse_memory_read_packet(
              "m1234,10", 8, 0x100, &request) == 0 &&
          request.address == 0x1234 && request.size == 0x10 &&
          request.data == NULL,
          "parse bounded memory read");
    check(uvdb_rsp_parse_memory_write_packet(
              "Mfffffffc,4:00112233",
              strlen("Mfffffffc,4:00112233"), 4, &request) == 0 &&
          request.address == UINT32_C(0xfffffffc) && request.size == 4 &&
          memcmp(request.data, "00112233", 8) == 0,
          "parse exact end-of-address-space write");
    check(uvdb_rsp_parse_memory_read_packet(
              "m0,0", strlen("m0,0"), 0, &request) == 0 &&
          request.address == 0 && request.size == 0,
          "accept zero-length memory read");
    check(uvdb_rsp_parse_memory_write_packet(
              "M0,0:", strlen("M0,0:"), 0, &request) == 0 &&
          request.address == 0 && request.size == 0 &&
          request.data != NULL,
          "accept zero-length memory write with exact empty payload");
    check(uvdb_rsp_parse_memory_read_packet(
              "m0,4", strlen("m0,4"), 4, &request) == 0 &&
          request.size == 4,
          "accept exact maximum read boundary");
    check(uvdb_rsp_parse_memory_write_packet(
              "M0,4:00112233", strlen("M0,4:00112233"), 4,
              &request) == 0 && request.size == 4,
          "accept exact maximum write boundary");
    const struct uvdb_rsp_memory_request boundary = request;
    check(uvdb_rsp_parse_memory_write_packet(
              "M0,4:00112233", strlen("M0,4:00112233"), 3,
              &request) < 0 &&
          memcmp(&request, &boundary, sizeof(request)) == 0,
          "reject one byte above maximum write boundary");

    static const char* malformed_reads[] = {
        "m", "m,1", "m0,", "m0", "m0,1,", "m0x,1", "m0,1x",
        "m000000000,1", "mffffffff,2", "m0,100000000",
    };
    for(size_t i = 0;
        i < sizeof(malformed_reads) / sizeof(malformed_reads[0]); ++i)
    {
        const struct uvdb_rsp_memory_request sentinel = {
            UINT32_C(0xaaaaaaaa), 0xbbbb,
            (const char*)(uintptr_t)0xcccc,
        };
        request = sentinel;
        check(uvdb_rsp_parse_memory_read_packet(
                  malformed_reads[i], strlen(malformed_reads[i]),
                  0x1000, &request) < 0 &&
              memcmp(&request, &sentinel, sizeof(request)) == 0,
              "reject malformed memory read without output mutation");
    }

    static const char* malformed_writes[] = {
        "M", "M0,1", "M0,1:", "M0,0:00", "M0,1:0", "M0,1:000",
        "M0,1:0x", "M0,1:00x", "Mfffffffe,3:000000",
        "M000000000,1:00", "M0,100000000:",
    };
    for(size_t i = 0;
        i < sizeof(malformed_writes) / sizeof(malformed_writes[0]); ++i)
    {
        const struct uvdb_rsp_memory_request sentinel = {
            UINT32_C(0xaaaaaaaa), 0xbbbb,
            (const char*)(uintptr_t)0xcccc,
        };
        request = sentinel;
        check(uvdb_rsp_parse_memory_write_packet(
                  malformed_writes[i], strlen(malformed_writes[i]),
                  0x1000, &request) < 0 &&
              memcmp(&request, &sentinel, sizeof(request)) == 0,
              "reject malformed memory write without output mutation");
    }

    const struct uvdb_rsp_memory_request sentinel = {
        UINT32_C(0xaaaaaaaa), 0xbbbb,
        (const char*)(uintptr_t)0xcccc,
    };
    request = sentinel;
    check(uvdb_rsp_parse_memory_read_packet(
              "m0,101", 6, 0x100, &request) < 0 &&
          memcmp(&request, &sentinel, sizeof(request)) == 0,
          "enforce reply-size limit before memory read");
}

static void test_hex_and_register_transactions(void)
{
    unsigned char output[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    check(uvdb_rsp_decode_hex_bytes(output, sizeof(output),
                                    "00112233", 8) == 0 &&
          output[0] == 0x00 && output[3] == 0x33,
          "decode complete hexadecimal byte span");
    const unsigned char sentinel[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    memcpy(output, sentinel, sizeof(output));
    check(uvdb_rsp_decode_hex_bytes(output, sizeof(output),
                                    "0011223x", 8) < 0 &&
          memcmp(output, sentinel, sizeof(output)) == 0,
          "bad final nibble leaves destination unchanged");
    check(uvdb_rsp_decode_hex_bytes(output, sizeof(output),
                                    "001122", 6) < 0 &&
          memcmp(output, sentinel, sizeof(output)) == 0,
          "truncated hex leaves destination unchanged");

    struct uvdb_rsp_core_registers original = {0};
    for(size_t i = 0; i < UVDB_RSP_CORE_REGISTER_COUNT; ++i)
        original.r[i] = UINT32_C(0x10203040) + (uint32_t)i;
    original.cpsr = UINT32_C(0x60000010);
    char encoded[UVDB_RSP_CORE_PACKET_HEX_SIZE];
    size_t encoded_size = 0;
    check(uvdb_rsp_encode_register_packet(
              encoded, sizeof(encoded), &original, NULL, 0,
              &encoded_size) == 0,
          "encode full-register hardening fixture");
    char packet[1 + UVDB_RSP_CORE_PACKET_HEX_SIZE];
    packet[0] = 'G';
    memcpy(packet + 1, encoded, sizeof(encoded));
    struct uvdb_rsp_core_registers parsed;
    memset(&parsed, 0xa5, sizeof(parsed));
    check(uvdb_rsp_parse_core_register_packet(
              packet, sizeof(packet), 0, &parsed) == 0 &&
          memcmp(&parsed, &original, sizeof(parsed)) == 0,
          "transactionally parse legacy full-register packet");

    const struct uvdb_rsp_core_registers register_sentinel = parsed;
    packet[sizeof(packet) - 1u] = 'z';
    check(uvdb_rsp_parse_core_register_packet(
              packet, sizeof(packet), 0, &parsed) < 0 &&
          memcmp(&parsed, &register_sentinel, sizeof(parsed)) == 0,
          "malformed CPSR leaves full register output unchanged");
    packet[sizeof(packet) - 1u] = encoded[sizeof(encoded) - 1u];
    check(uvdb_rsp_parse_core_register_packet(
              packet, sizeof(packet) - 1u, 0, &parsed) < 0 &&
          memcmp(&parsed, &register_sentinel, sizeof(parsed)) == 0,
          "truncated G packet leaves full register output unchanged");
}

static void test_breakpoint_and_xfer_packets(void)
{
    int insert = -1;
    uint32_t address = UINT32_MAX;
    size_t kind = SIZE_MAX;
    check(uvdb_rsp_parse_software_breakpoint_packet(
              "Z0,0,2", strlen("Z0,0,2"), &insert, &address, &kind) == 0 &&
          insert == 1 && address == 0 && kind == 2,
          "accept minimum-length software-breakpoint insertion");
    check(uvdb_rsp_parse_software_breakpoint_packet(
              "z0,0,2", strlen("z0,0,2"), &insert, &address, &kind) == 0 &&
          insert == 0 && address == 0 && kind == 2,
          "accept minimum-length software-breakpoint removal");
    check(uvdb_rsp_parse_software_breakpoint_packet(
              "Z0,81001234,2", 13, &insert, &address, &kind) == 0 &&
          insert == 1 && address == UINT32_C(0x81001234) && kind == 2,
          "parse exact software-breakpoint packet");
    const int old_insert = insert;
    const uint32_t old_address = address;
    const size_t old_kind = kind;
    check(uvdb_rsp_parse_software_breakpoint_packet(
              "Z0,81001234,2junk", 17, &insert, &address, &kind) < 0 &&
          insert == old_insert && address == old_address && kind == old_kind,
          "reject trailing breakpoint garbage transactionally");

    struct uvdb_rsp_xfer_range range = {UINT64_MAX, UINT64_MAX};
    check(uvdb_rsp_parse_xfer_range("10,20", 5, &range) == 0 &&
          range.offset == 0x10 && range.length == 0x20,
          "parse exact qXfer range");
    const struct uvdb_rsp_xfer_range range_sentinel = range;
    const char* malformed[] = {
        "", ",", "1,", ",1", "1", "1,2,3", "x,1",
        "10000000000000000,1", "1,10000000000000000",
        "ffffffffffffffff,1",
    };
    for(size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
    {
        range = range_sentinel;
        check(uvdb_rsp_parse_xfer_range(
                  malformed[i], strlen(malformed[i]), &range) < 0 &&
              memcmp(&range, &range_sentinel, sizeof(range)) == 0,
              "reject malformed qXfer range transactionally");
    }

    uint32_t file_result = UINT32_C(0xa5a5a5a5);
    check(uvdb_rsp_parse_fileio_result_packet(
              "F2a", strlen("F2a"), &file_result) == 0 &&
          file_result == 0x2a,
          "parse positive file-I/O result");

    static const char interrupted_packet[] = "F-1,2,C;abc,;xyz";
    struct uvdb_rsp_fileio_result parsed_fileio;
    memset(&parsed_fileio, 0xcc, sizeof(parsed_fileio));
    check(uvdb_rsp_parse_fileio_packet(
              interrupted_packet, sizeof(interrupted_packet) - 1u,
              &parsed_fileio) == 0 &&
          parsed_fileio.result == UINT32_MAX &&
          parsed_fileio.has_error_number == 1 &&
          parsed_fileio.error_number == 2 &&
          parsed_fileio.interrupted == 1 &&
          parsed_fileio.has_attachment == 1 &&
          parsed_fileio.attachment_size == strlen("abc,;xyz") &&
          !memcmp(parsed_fileio.attachment, "abc,;xyz", strlen("abc,;xyz")),
          "parse literal Ctrl-C and bounded arbitrary attachment");
    check(uvdb_rsp_parse_fileio_result_packet(
              interrupted_packet, sizeof(interrupted_packet) - 1u,
              &file_result) == 0 && file_result == UINT32_MAX,
          "compatibility parser accepts standard interrupted reply");

    struct uvdb_fileio_transition transition = {
        UINT32_MAX, UVDB_FILEIO_ACTION_FAIL_CLOSED, 99u, 99,
    };
    check(uvdb_fileio_transition_decide(
              &parsed_fileio, UVDB_FILEIO_CONTEXT_REAL_STOP,
              &transition) == 0 &&
          transition.result == UINT32_MAX &&
          transition.action == UVDB_FILEIO_ACTION_REPORT_INTERRUPT &&
          transition.stop_reply_count == 1u &&
          transition.stop_signal == UVDB_FILEIO_SIGINT,
          "real all-stop maps Ctrl-C to exactly one T02 without resume");
    check(uvdb_fileio_transition_decide(
              &parsed_fileio, UVDB_FILEIO_CONTEXT_UNSTOPPED,
              &transition) == 0 &&
          transition.action == UVDB_FILEIO_ACTION_FAIL_CLOSED &&
          transition.stop_reply_count == 0u && transition.stop_signal == 0,
          "synthetic unstopped syscall rejects Ctrl-C instead of fake stop");

    check(uvdb_rsp_parse_fileio_packet(
              "F1;", strlen("F1;"), &parsed_fileio) == 0 &&
          parsed_fileio.result == 1 && parsed_fileio.has_attachment == 1 &&
          parsed_fileio.attachment_size == 0,
          "empty optional attachment is represented exactly");
    check(uvdb_rsp_parse_fileio_packet(
              "F1;data", strlen("F1;data"), &parsed_fileio) == 0 &&
          parsed_fileio.attachment_size == 4,
          "attachment after bare result is accepted");
    check(uvdb_fileio_transition_decide(
              &parsed_fileio, UVDB_FILEIO_CONTEXT_UNSTOPPED,
              &transition) == 0 &&
          transition.action == UVDB_FILEIO_ACTION_RESUME &&
          transition.result == 1 && transition.stop_reply_count == 0u,
          "ordinary File-I/O reply retains the existing resume transition");
    const uint32_t file_sentinel = file_result;
    static const char* bad_fileio[] = {
        "F", "F-", "Fz", "F1,", "F1,,C", "F1,2,0",
        "F1,2,c", "F1,2,Cx", "F1,2,C,4", "F-80000001",
    };
    for(size_t i = 0;
        i < sizeof(bad_fileio) / sizeof(bad_fileio[0]); ++i)
    {
        file_result = file_sentinel;
        check(uvdb_rsp_parse_fileio_result_packet(
                  bad_fileio[i], strlen(bad_fileio[i]), &file_result) < 0 &&
              file_result == file_sentinel,
              "reject malformed file-I/O result transactionally");
    }
}

static void test_deterministic_fuzz(void)
{
    unsigned char bytes[512];
    for(size_t iteration = 0; iteration < 25000u; ++iteration)
    {
        const size_t size = next_random() % sizeof(bytes);
        for(size_t i = 0; i < size; ++i)
            bytes[i] = (unsigned char)next_random();

        struct uvdb_rsp_frame frame = {
            SIZE_MAX, SIZE_MAX, SIZE_MAX, UINT_MAX,
        };
        int frame_result = uvdb_rsp_scan_frame(
            bytes, size, 256u, &frame);
        check(frame_result >= UVDB_RSP_FRAME_DISCARD &&
                  frame_result <= UVDB_RSP_FRAME_COMPLETE,
              "fuzz frame result is in enum range");
        if(frame_result == UVDB_RSP_FRAME_DISCARD)
            check(frame.consumed_size > 0 && frame.consumed_size <= size,
                  "fuzz discard always makes bounded progress");
        else if(frame_result == UVDB_RSP_FRAME_INCOMPLETE)
            check(frame.consumed_size == 0,
                  "fuzz incomplete frame consumes nothing");
        else
            check(frame.payload_offset <= frame.consumed_size &&
                      frame.payload_size <= 256u &&
                      frame.consumed_size <= size &&
                      frame.request_nack == 0u,
                  "fuzz complete frame spans stay bounded");
        check(frame.request_nack <= 1u,
              "fuzz frame NACK disposition is boolean");

        const struct uvdb_rsp_memory_request memory_sentinel = {
            UINT32_C(0x13572468), 0x2468,
            (const char*)(uintptr_t)0x1234,
        };
        struct uvdb_rsp_memory_request memory = memory_sentinel;
        if(uvdb_rsp_parse_memory_read_packet(
               (const char*)bytes, size, 256u, &memory) < 0)
            check(memcmp(&memory, &memory_sentinel, sizeof(memory)) == 0,
                  "fuzz invalid read does not mutate output");
        memory = memory_sentinel;
        if(uvdb_rsp_parse_memory_write_packet(
               (const char*)bytes, size, 256u, &memory) < 0)
            check(memcmp(&memory, &memory_sentinel, sizeof(memory)) == 0,
                  "fuzz invalid write does not mutate output");

        uint32_t thread = UINT32_C(0x89abcdef);
        const uint32_t thread_sentinel = thread;
        if(uvdb_rsp_parse_u32_hex((const char*)bytes, size, &thread) < 0)
            check(thread == thread_sentinel,
                  "fuzz invalid thread hex does not mutate output");

        const struct uvdb_rsp_fileio_result fileio_sentinel = {
            .result = UINT32_C(0x11223344),
            .error_number = UINT32_C(0x55667788),
            .has_error_number = 7,
            .interrupted = 9,
            .has_attachment = 11,
            .attachment = (const char*)(uintptr_t)0x1234,
            .attachment_size = 0x5678,
        };
        struct uvdb_rsp_fileio_result fileio = fileio_sentinel;
        if(uvdb_rsp_parse_fileio_packet(
               (const char*)bytes, size, &fileio) < 0)
            check(memcmp(&fileio, &fileio_sentinel, sizeof(fileio)) == 0,
                  "fuzz invalid file-I/O does not mutate output");

        int insert = 7;
        uint32_t address = UINT32_C(0x89abcdef);
        size_t kind = SIZE_MAX;
        if(uvdb_rsp_parse_software_breakpoint_packet(
               (const char*)bytes, size, &insert, &address, &kind) < 0)
            check(insert == 7 && address == UINT32_C(0x89abcdef) &&
                      kind == SIZE_MAX,
                  "fuzz invalid breakpoint does not mutate output");
    }
}

int main(void)
{
    test_frames();
    test_memory_packets();
    test_hex_and_register_transactions();
    test_breakpoint_and_xfer_packets();
    test_deterministic_fuzz();
    if(failures)
        return 1;
    puts("PASS: bounded RSP parser and deterministic fuzz hardening");
    return 0;
}
