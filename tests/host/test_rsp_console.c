#include <stdint.h>
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

static void test_empty_payload(void)
{
    char output[2] = {'?', '?'};
    size_t output_size = 0;
    check(uvdb_rsp_encode_console_payload(output, sizeof(output), NULL, 0,
                                           &output_size) == 0,
          "empty payload accepted");
    check(output_size == 1 && output[0] == 'O',
          "empty payload shape");
    check(output[1] == '?', "encoder does not append terminator");
}

static void test_binary_payload(void)
{
    const uint8_t input[] = {0x00, 0x03, '$', '#', '}', 0x7f, 0x80, 0xff};
    const char expected[] = "O000324237d7f80ff";
    char output[sizeof(expected)];
    memset(output, '?', sizeof(output));
    size_t output_size = 0;
    check(uvdb_rsp_encode_console_payload(output, sizeof(output), input,
                                           sizeof(input), &output_size) == 0,
          "binary payload accepted");
    check(output_size == sizeof(expected) - 1,
          "binary payload required size");
    check(memcmp(output, expected, output_size) == 0,
          "binary payload exact lowercase hex");
    check(output[output_size] == '?', "binary payload has no terminator");
}

static void test_max_queue_record(void)
{
    uint8_t input[128];
    char output[1 + sizeof(input) * 2];
    for(size_t i = 0; i < sizeof(input); ++i)
        input[i] = (uint8_t)i;
    size_t output_size = 0;
    check(uvdb_rsp_encode_console_payload(output, sizeof(output), input,
                                           sizeof(input), &output_size) == 0,
          "maximum queue record accepted");
    check(output_size == sizeof(output) && output[0] == 'O',
          "maximum queue record exact size");
    check(memcmp(output + 1, "00010203", 8) == 0,
          "maximum queue record prefix");
    check(memcmp(output + sizeof(output) - 8, "7c7d7e7f", 8) == 0,
          "maximum queue record suffix");
}

static void test_failures(void)
{
    const uint8_t input[] = {0xab, 0xcd};
    char output[5] = {'!', '!', '!', '!', '!'};
    size_t output_size = 0;
    check(uvdb_rsp_encode_console_payload(output, sizeof(output) - 1, input,
                                           sizeof(input), &output_size) < 0,
          "short capacity rejected");
    check(output_size == sizeof(output),
          "short capacity reports required size");
    check(output[0] == '!', "short capacity leaves output untouched");
    check(uvdb_rsp_encode_console_payload(NULL, 0, input, sizeof(input),
                                           &output_size) < 0 &&
              output_size == sizeof(output),
          "NULL output rejected with required size");
    check(uvdb_rsp_encode_console_payload(output, sizeof(output), NULL, 1,
                                           &output_size) < 0 &&
              output_size == 3,
          "NULL nonempty input rejected");
    check(uvdb_rsp_encode_console_payload(output, sizeof(output), input,
                                           sizeof(input), NULL) < 0,
          "NULL output-size rejected");
}

static void test_overlap_rejected(void)
{
    unsigned char storage[32];
    memset(storage, 0x5a, sizeof(storage));
    size_t output_size = 0;
    check(uvdb_rsp_encode_console_payload((char*)storage, sizeof(storage),
                                           storage, 4, &output_size) < 0 &&
              output_size == 9,
          "same-buffer overlap rejected");
    check(storage[0] == 0x5a, "same-buffer rejection writes nothing");

    check(uvdb_rsp_encode_console_payload((char*)storage + 6,
                                           sizeof(storage) - 6,
                                           storage + 8, 4,
                                           &output_size) < 0 &&
              output_size == 9,
          "output-before-input overlap rejected");
    check(uvdb_rsp_encode_console_payload((char*)storage + 10,
                                           sizeof(storage) - 10,
                                           storage + 8, 4,
                                           &output_size) < 0 &&
              output_size == 9,
          "input-before-output overlap rejected");

    storage[0] = 0xab;
    check(uvdb_rsp_encode_console_payload((char*)storage + 1,
                                           sizeof(storage) - 1,
                                           storage, 1,
                                           &output_size) == 0,
          "adjacent spans accepted");
    check(output_size == 3 && memcmp(storage + 1, "Oab", 3) == 0,
          "adjacent span exact output");

    check(uvdb_rsp_encode_console_payload((char*)storage, SIZE_MAX,
                                           storage + 16,
                                           (SIZE_MAX / 2u) + 1u,
                                           &output_size) < 0 &&
              output_size == SIZE_MAX,
          "required-size overflow rejected before span access");
}

int main(void)
{
    test_empty_payload();
    test_binary_payload();
    test_max_queue_record();
    test_failures();
    test_overlap_rejected();
    if(failures)
        return 1;
    puts("PASS: GDB remote-console payload serialization");
    return 0;
}
