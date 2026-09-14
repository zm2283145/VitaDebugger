#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_exclusive_step.h"

#define BASE UINT32_C(0x1000)
#define CPSR_T (UINT32_C(1) << 5)

struct memory_image
{
    unsigned char bytes[UVDB_EXCLUSIVE_STEP_MAX_BYTES];
    size_t size;
    uint32_t base;
    unsigned int calls;
    unsigned int fail_call;
    unsigned int partial_call;
};

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void image_reset(struct memory_image* image)
{
    memset(image, 0, sizeof(*image));
    image->base = BASE;
}

static void append_u16(struct memory_image* image, uint16_t value)
{
    check(image->size + 2u <= sizeof(image->bytes), "fixture halfword fits");
    image->bytes[image->size++] = (unsigned char)value;
    image->bytes[image->size++] = (unsigned char)(value >> 8);
}

static void append_u32(struct memory_image* image, uint32_t value)
{
    append_u16(image, (uint16_t)value);
    append_u16(image, (uint16_t)(value >> 16));
}

static void append_thumb32(
    struct memory_image* image,
    uint16_t first,
    uint16_t second)
{
    append_u16(image, first);
    append_u16(image, second);
}

static int image_read(
    void* context,
    uint32_t address,
    unsigned char* destination,
    size_t size)
{
    struct memory_image* image = context;
    size_t offset;

    image->calls++;
    if(image->calls == image->fail_call)
        return -1;
    if(address < image->base)
        return -1;
    offset = (size_t)(address - image->base);
    if(offset > image->size || size > image->size - offset)
        return -1;
    memcpy(destination, image->bytes + offset, size);
    if(image->calls == image->partial_call)
        return size == 0u ? 0 : (int)size - 1;
    return (int)size;
}

static int scan_with(
    struct memory_image* image,
    uint32_t pc,
    uint32_t cpsr,
    unsigned int instruction_limit,
    unsigned int byte_limit,
    struct uvdb_exclusive_step_target* target)
{
    const struct uvdb_exclusive_step_limits limits = {
        instruction_limit, byte_limit
    };
    return uvdb_exclusive_step_scan(
        image_read, image, pc, cpsr, &limits, target);
}

static void expect_success(
    struct memory_image* image,
    uint32_t cpsr,
    uint32_t address,
    unsigned int breakpoint_size,
    unsigned int instructions,
    unsigned int bytes,
    const char* name)
{
    struct uvdb_exclusive_step_target target = {0, 0, 0, 0};
    int result = scan_with(
        image, BASE, cpsr, UVDB_EXCLUSIVE_STEP_MAX_INSTRUCTIONS,
        UVDB_EXCLUSIVE_STEP_MAX_BYTES, &target);
    check(result == 1 && target.address == address &&
          target.breakpoint_size == breakpoint_size &&
          target.scanned_instructions == instructions &&
          target.scanned_bytes == bytes, name);
}

static void expect_reject(
    struct memory_image* image,
    uint32_t pc,
    uint32_t cpsr,
    unsigned int instruction_limit,
    unsigned int byte_limit,
    const char* name)
{
    struct uvdb_exclusive_step_target target = {
        UINT32_C(0xaaaaaaaa), 0xbbbbbbbbU, 0xccccccccU, 0xddddddddU
    };
    const struct uvdb_exclusive_step_target sentinel = target;
    int result = scan_with(image, pc, cpsr, instruction_limit, byte_limit,
                           &target);
    check(result == 0 && memcmp(&target, &sentinel, sizeof(target)) == 0,
          name);
}

static void test_arm_successes(void)
{
    struct memory_image image;

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f)); /* ldrex r0,[r1] */
    append_u32(&image, UINT32_C(0xe1a02002)); /* mov r2,r2 */
    append_u32(&image, UINT32_C(0xe1813f92)); /* strex r3,r2,[r1] */
    expect_success(&image, 0, BASE + 12u, 4u, 3u, 12u,
                   "A32 LDREX/STREX with straight-line body");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1d32f9f)); /* ldrexb r2,[r3] */
    append_u32(&image, UINT32_C(0xe1c30f91)); /* strexb r0,r1,[r3] */
    expect_success(&image, 0, BASE + 8u, 4u, 2u, 8u,
                   "A32 byte-exclusive family");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1f54f9f)); /* ldrexh r4,[r5] */
    append_u32(&image, UINT32_C(0xe1e53f94)); /* strexh r3,r4,[r5] */
    expect_success(&image, 0, BASE + 8u, 4u, 2u, 8u,
                   "A32 halfword-exclusive family");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1b86f9f)); /* ldrexd r6,r7,[r8] */
    append_u32(&image, UINT32_C(0xe1a86f96)); /* strexd r6,r6,r7,[r8] invalid status */
    expect_reject(&image, BASE, 0, 2u, 8u,
                  "A32 double store rejects status/data overlap");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1b86f9f)); /* ldrexd r6,r7,[r8] */
    append_u32(&image, UINT32_C(0xe1a85f96)); /* strexd r5,r6,r7,[r8] */
    expect_success(&image, 0, BASE + 8u, 4u, 2u, 8u,
                   "A32 doubleword-exclusive family");

    image_reset(&image);
    append_u32(&image, UINT32_C(0x01910f9f)); /* ldrexeq r0,[r1] */
    append_u32(&image, UINT32_C(0xe1813f92));
    expect_success(&image, UINT32_C(1) << 30, BASE + 8u, 4u, 2u, 8u,
                   "executed conditional A32 LDREX starts a sequence");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xf57ff01f)); /* clrex */
    expect_success(&image, 0, BASE + 8u, 4u, 2u, 8u,
                   "A32 CLREX terminates a sequence");
}

static void test_thumb_successes(void)
{
    struct memory_image image;

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_u16(&image, UINT16_C(0xbf00));
    append_thumb32(&image, UINT16_C(0xf3af), UINT16_C(0x8000));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
    expect_success(&image, CPSR_T, BASE + 14u, 2u, 4u, 14u,
                   "Thumb LDREX/STREX across 16- and 32-bit body");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe8d3), UINT16_C(0x2f4f));
    append_thumb32(&image, UINT16_C(0xe8c3), UINT16_C(0x1f40));
    expect_success(&image, CPSR_T, BASE + 8u, 2u, 2u, 8u,
                   "Thumb byte-exclusive family");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe8d5), UINT16_C(0x4f5f));
    append_thumb32(&image, UINT16_C(0xe8c5), UINT16_C(0x4f53));
    expect_success(&image, CPSR_T, BASE + 8u, 2u, 2u, 8u,
                   "Thumb halfword-exclusive family");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe8d8), UINT16_C(0x677f));
    append_thumb32(&image, UINT16_C(0xe8c8), UINT16_C(0x6774));
    expect_success(&image, CPSR_T, BASE + 8u, 2u, 2u, 8u,
                   "Thumb doubleword-exclusive family");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xf3bf), UINT16_C(0x8f2f));
    expect_success(&image, CPSR_T, BASE + 8u, 2u, 2u, 8u,
                   "Thumb CLREX terminates a sequence");
}

static void test_mismatch_nested_and_invalid(void)
{
    struct memory_image image;

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1823f92));
    expect_reject(&image, BASE, 0, 2u, 8u, "A32 mismatched base rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1c10f92));
    expect_reject(&image, BASE, 0, 2u, 8u, "A32 mismatched family rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1920f9f));
    expect_reject(&image, BASE, 0, 2u, 8u, "A32 nested exclusive load rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe19f0f9f));
    expect_reject(&image, BASE, 0, 1u, 4u, "A32 LDREX with PC base rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1b17f9f));
    expect_reject(&image, BASE, 0, 1u, 4u, "A32 LDREXD with odd pair rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0x01910f9f));
    expect_reject(&image, BASE, 0, 1u, 4u,
                  "condition-failed initial A32 LDREX rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0x01813f92));
    expect_reject(&image, BASE, UINT32_C(1) << 30, 2u, 8u,
                  "conditional A32 STREX rejects after mutable flags");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe842), UINT16_C(0x2300));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb mismatched base rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe8c1), UINT16_C(0x2f43));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb mismatched family rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe8d1), UINT16_C(0x2f4f));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb nested exclusive load rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe8d1), UINT16_C(0xdf7f));
    expect_reject(&image, BASE, CPSR_T, 1u, 4u,
                  "Thumb LDREXD with SP data register rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0xdf00));
    expect_reject(&image, BASE, CPSR_T, 1u, 4u,
                  "Thumb LDREX with SP destination rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2100));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb STREX status/base overlap rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2d00));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb STREX with SP status rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0xd200));
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb STREX with SP data rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1a00000));
    expect_reject(&image, BASE, 0, 1u, 4u,
                  "ordinary A32 start rejects");
    image_reset(&image);
    append_u16(&image, UINT16_C(0xbf00));
    expect_reject(&image, BASE, CPSR_T, 1u, 2u,
                  "ordinary Thumb start rejects");
}

static void test_control_flow_and_blocking(void)
{
    static const uint32_t arm_dangerous[] = {
        UINT32_C(0xea000000), UINT32_C(0xe12fff10),
        UINT32_C(0xe591f000), UINT32_C(0xe1a0f000),
        UINT32_C(0xe8918000), UINT32_C(0xfa000000),
        UINT32_C(0xef000000), UINT32_C(0xe320f002),
        UINT32_C(0xe320f003)
    };
    static const uint16_t thumb16_dangerous[] = {
        UINT16_C(0xe000), UINT16_C(0xd000), UINT16_C(0x4700),
        UINT16_C(0xbd00), UINT16_C(0xb100), UINT16_C(0xdf00),
        UINT16_C(0xbf20), UINT16_C(0xbf30), UINT16_C(0xbf08)
    };
    static const uint16_t thumb32_dangerous[][2] = {
        {UINT16_C(0xf000), UINT16_C(0xb800)},
        {UINT16_C(0xf8d0), UINT16_C(0xf000)},
        {UINT16_C(0xe8d0), UINT16_C(0xf000)},
        {UINT16_C(0xea4f), UINT16_C(0x0f00)},
        {UINT16_C(0xf3af), UINT16_C(0x8002)},
        {UINT16_C(0xf3af), UINT16_C(0x8003)}
    };
    struct memory_image image;
    size_t index;

    for(index = 0; index < sizeof(arm_dangerous) / sizeof(arm_dangerous[0]);
        ++index)
    {
        image_reset(&image);
        append_u32(&image, UINT32_C(0xe1910f9f));
        append_u32(&image, arm_dangerous[index]);
        append_u32(&image, UINT32_C(0xe1813f92));
        expect_reject(&image, BASE, 0, 3u, 12u,
                      "A32 branch, PC writer, syscall, or wait rejects");
    }
    for(index = 0;
        index < sizeof(thumb16_dangerous) / sizeof(thumb16_dangerous[0]);
        ++index)
    {
        image_reset(&image);
        append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
        append_u16(&image, thumb16_dangerous[index]);
        append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
        expect_reject(&image, BASE, CPSR_T, 3u, 10u,
                      "Thumb16 branch, PC writer, syscall, wait, or IT rejects");
    }
    for(index = 0;
        index < sizeof(thumb32_dangerous) / sizeof(thumb32_dangerous[0]);
        ++index)
    {
        image_reset(&image);
        append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
        append_thumb32(&image, thumb32_dangerous[index][0],
                       thumb32_dangerous[index][1]);
        append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
        expect_reject(&image, BASE, CPSR_T, 3u, 12u,
                      "Thumb32 branch, PC writer, or wait rejects");
    }
}

static void test_bounds_reads_and_state(void)
{
    struct memory_image image;
    struct uvdb_exclusive_step_target target = {0, 0, 0, 0};
    struct uvdb_exclusive_step_limits limits = {2u, 8u};

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1a00000));
    append_u32(&image, UINT32_C(0xe1813f92));
    expect_reject(&image, BASE, 0, 2u, 12u,
                  "A32 instruction bound exhausts before terminator");
    image.calls = 0;
    expect_reject(&image, BASE, 0, 3u, 11u,
                  "A32 byte bound exhausts before terminator");
    image.calls = 0;
    expect_success(&image, 0, BASE + 12u, 4u, 3u, 12u,
                   "A32 exact bounds include terminator");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_u16(&image, UINT16_C(0xbf00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
    expect_reject(&image, BASE, CPSR_T, 2u, 10u,
                  "Thumb instruction bound exhausts before terminator");
    image.calls = 0;
    expect_reject(&image, BASE, CPSR_T, 3u, 9u,
                  "Thumb byte bound refuses partial 32-bit instruction");
    check(image.calls == 4u,
          "Thumb byte bound does not read terminator second halfword");
    image.calls = 0;
    expect_success(&image, CPSR_T, BASE + 10u, 2u, 3u, 10u,
                   "Thumb exact bounds include terminator");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1813f92));
    image.fail_call = 1u;
    expect_reject(&image, BASE, 0, 2u, 8u, "first A32 read failure rejects");
    image.calls = 0;
    image.fail_call = 2u;
    expect_reject(&image, BASE, 0, 2u, 8u, "later A32 read failure rejects");
    image.calls = 0;
    image.fail_call = 0u;
    image.partial_call = 1u;
    expect_reject(&image, BASE, 0, 2u, 8u, "partial A32 read rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
    image.fail_call = 2u;
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb start second-half read failure rejects");
    image.calls = 0;
    image.fail_call = 4u;
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "Thumb terminator second-half read failure rejects");
    image.calls = 0;
    image.fail_call = 0u;
    image.partial_call = 3u;
    expect_reject(&image, BASE, CPSR_T, 2u, 8u,
                  "partial Thumb read rejects");

    image_reset(&image);
    append_thumb32(&image, UINT16_C(0xe851), UINT16_C(0x0f00));
    append_thumb32(&image, UINT16_C(0xe841), UINT16_C(0x2300));
    expect_reject(&image, BASE + 1u, CPSR_T, 2u, 8u,
                  "misaligned Thumb PC rejects");
    expect_reject(&image, BASE, CPSR_T | (UINT32_C(8) << 8), 2u, 8u,
                  "active Thumb ITSTATE rejects");
    expect_reject(&image, BASE, CPSR_T | (UINT32_C(1) << 25), 2u, 8u,
                  "split high Thumb ITSTATE rejects");
    expect_reject(&image, BASE, CPSR_T | (UINT32_C(1) << 24), 2u, 8u,
                  "Jazelle or ThumbEE state rejects");
    expect_reject(&image, BASE, CPSR_T | (UINT32_C(1) << 9), 2u, 8u,
                  "big-endian data state rejects");

    image_reset(&image);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1813f92));
    expect_reject(&image, BASE + 2u, 0, 2u, 8u,
                  "misaligned A32 PC rejects");
    expect_reject(&image, BASE, 0, 0u, 8u, "zero instruction limit rejects");
    expect_reject(&image, BASE, 0, 2u, 0u, "zero byte limit rejects");
    expect_reject(&image, BASE, 0,
                  UVDB_EXCLUSIVE_STEP_MAX_INSTRUCTIONS + 1u, 8u,
                  "instruction limit over hard cap rejects");
    expect_reject(&image, BASE, 0, 2u,
                  UVDB_EXCLUSIVE_STEP_MAX_BYTES + 1u,
                  "byte limit over hard cap rejects");
    check(uvdb_exclusive_step_scan(NULL, &image, BASE, 0, &limits,
                                   &target) == 0,
          "null reader rejects");
    check(uvdb_exclusive_step_scan(image_read, &image, BASE, 0, NULL,
                                   &target) == 0,
          "null limits reject");
    check(uvdb_exclusive_step_scan(image_read, &image, BASE, 0, &limits,
                                   NULL) == 0,
          "null target rejects");

    image_reset(&image);
    image.base = UINT32_C(0xfffffff8);
    append_u32(&image, UINT32_C(0xe1910f9f));
    append_u32(&image, UINT32_C(0xe1813f92));
    expect_reject(&image, UINT32_C(0xfffffff8), 0, 2u, 8u,
                  "post-sequence address wrap rejects");
}

int main(void)
{
    test_arm_successes();
    test_thumb_successes();
    test_mismatch_nested_and_invalid();
    test_control_flow_and_blocking();
    test_bounds_reads_and_state();

    if(failures != 0)
        return 1;
    puts("PASS: bounded A32 and Thumb exclusive-step scanner");
    return 0;
}
