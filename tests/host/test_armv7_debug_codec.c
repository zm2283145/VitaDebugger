#include <stdint.h>
#include <stdio.h>

#include "vd_armv7_debug_codec.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void test_exec(void)
{
    struct vd_armv7_debug_encoding encoding = {UINT32_C(0xaaaaaaaa),
                                                UINT32_C(0xbbbbbbbb)};

    check(vd_armv7_encode_linked_exec(UINT32_C(0x12345678), 4,
                                       &encoding) == VD_ARMV7_DEBUG_OK,
          "ARM breakpoint accepted");
    check(encoding.value == UINT32_C(0x12345678),
          "ARM breakpoint BVR");
    check(encoding.control == UINT32_C(0x001501e5),
          "ARM linked address BCR exact value");

    check(vd_armv7_encode_linked_exec(UINT32_C(0x12345678), 2,
                                       &encoding) == VD_ARMV7_DEBUG_OK,
          "low Thumb breakpoint accepted");
    check(encoding.value == UINT32_C(0x12345678),
          "low Thumb breakpoint aligned BVR");
    check(encoding.control == UINT32_C(0x00150065),
          "low Thumb linked address BCR exact value");

    check(vd_armv7_encode_linked_exec(UINT32_C(0x1234567a), 2,
                                       &encoding) == VD_ARMV7_DEBUG_OK,
          "high Thumb breakpoint accepted");
    check(encoding.value == UINT32_C(0x12345678),
          "high Thumb breakpoint aligned BVR");
    check(encoding.control == UINT32_C(0x00150185),
          "high Thumb linked address BCR exact value");

    check(vd_armv7_encode_linked_exec(UINT32_C(0x1001), 2,
                                       &encoding) ==
              VD_ARMV7_DEBUG_INVALID_ALIGNMENT,
          "odd Thumb breakpoint rejected");
    check(vd_armv7_encode_linked_exec(UINT32_C(0x1002), 4,
                                       &encoding) ==
              VD_ARMV7_DEBUG_INVALID_ALIGNMENT,
          "unaligned ARM breakpoint rejected");
    check(vd_armv7_encode_linked_exec(UINT32_C(0x1000), 1,
                                       &encoding) ==
              VD_ARMV7_DEBUG_INVALID_LENGTH,
          "one-byte execution breakpoint rejected");
    check(vd_armv7_encode_linked_exec(UINT32_C(0x1000), 8,
                                       &encoding) ==
              VD_ARMV7_DEBUG_INVALID_LENGTH,
          "eight-byte execution breakpoint rejected");
    check(encoding.value == 0 && encoding.control == 0,
          "invalid execution request clears output");
    check(vd_armv7_encode_linked_exec(UINT32_C(0x1000), 4, 0) ==
              VD_ARMV7_DEBUG_INVALID_ARGUMENT,
          "NULL execution output rejected");
}

static void test_context(void)
{
    struct vd_armv7_debug_encoding encoding;

    check(vd_armv7_encode_linked_context(UINT32_C(0x89abcdef),
                                          &encoding) ==
              VD_ARMV7_DEBUG_OK,
          "context breakpoint accepted");
    check(encoding.value == UINT32_C(0x89abcdef),
          "context BVR5 retains full context ID");
    check(encoding.control == UINT32_C(0x003001e7),
          "linked context BCR5 exact value");
    check(vd_armv7_encode_linked_context(0, 0) ==
              VD_ARMV7_DEBUG_INVALID_ARGUMENT,
          "NULL context output rejected");
}

static void test_watch(void)
{
    struct vd_armv7_debug_encoding encoding;

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 1,
                                        VD_ARMV7_WATCH_READ,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "read watchpoint accepted");
    check(encoding.value == UINT32_C(0x2000),
          "read watchpoint WVR");
    check(encoding.control == UINT32_C(0x0015002d),
          "linked read WCR exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 4,
                                        VD_ARMV7_WATCH_READ,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "word read watchpoint accepted");
    check(encoding.control == UINT32_C(0x001501ed),
          "linked word-read WCR exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2001), 2,
                                        VD_ARMV7_WATCH_WRITE,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "within-word unaligned halfword watchpoint accepted");
    check(encoding.value == UINT32_C(0x2000),
          "halfword watchpoint aligned WVR");
    check(encoding.control == UINT32_C(0x001500d5),
          "linked write WCR exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 4,
                                        VD_ARMV7_WATCH_WRITE,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "word write watchpoint accepted");
    check(encoding.control == UINT32_C(0x001501f5),
          "linked word-write WCR exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 4,
                                        VD_ARMV7_WATCH_ACCESS,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "access watchpoint accepted");
    check(encoding.control == UINT32_C(0x001501fd),
          "linked access WCR exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2003), 1,
                                        VD_ARMV7_WATCH_READ,
                                        &encoding) == VD_ARMV7_DEBUG_OK,
          "last byte watchpoint accepted");
    check(encoding.control == UINT32_C(0x0015010d),
          "last byte BAS exact value");

    check(vd_armv7_encode_linked_watch(UINT32_C(0x2003), 2,
                                        VD_ARMV7_WATCH_WRITE,
                                        &encoding) ==
              VD_ARMV7_DEBUG_CROSSES_WORD,
          "cross-word halfword watchpoint rejected");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2001), 4,
                                        VD_ARMV7_WATCH_ACCESS,
                                        &encoding) ==
              VD_ARMV7_DEBUG_CROSSES_WORD,
          "cross-word word watchpoint rejected");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 0,
                                        VD_ARMV7_WATCH_READ,
                                        &encoding) ==
              VD_ARMV7_DEBUG_INVALID_LENGTH,
          "zero-length watchpoint rejected");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 3,
                                        VD_ARMV7_WATCH_READ,
                                        &encoding) ==
              VD_ARMV7_DEBUG_INVALID_LENGTH,
          "three-byte watchpoint rejected");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 1,
                                        (enum vd_armv7_watch_access)0,
                                        &encoding) ==
              VD_ARMV7_DEBUG_INVALID_ACCESS,
          "reserved watchpoint access rejected");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 1,
                                        (enum vd_armv7_watch_access)4,
                                        &encoding) ==
              VD_ARMV7_DEBUG_INVALID_ACCESS,
          "out-of-range watchpoint access rejected");
    check(encoding.value == 0 && encoding.control == 0,
          "invalid watchpoint request clears output");
    check(vd_armv7_encode_linked_watch(UINT32_C(0x2000), 1,
                                        VD_ARMV7_WATCH_READ, 0) ==
              VD_ARMV7_DEBUG_INVALID_ARGUMENT,
          "NULL watchpoint output rejected");
}

static void test_fsr(void)
{
    check(vd_armv7_fsr_status(UINT32_C(0x00000002)) == UINT32_C(0x2),
          "low FSR debug status extraction");
    check(vd_armv7_fsr_status(UINT32_C(0x0000040f)) == UINT32_C(0x1f),
          "split FSR status extraction");
    check(vd_armv7_fsr_status(UINT32_C(0xfffffbf2)) == UINT32_C(0x2),
          "unrelated FSR bits ignored");
    check(vd_armv7_fsr_is_debug_event(UINT32_C(0x00000002)),
          "debug FSR recognized");
    check(vd_armv7_fsr_is_debug_event(UINT32_C(0x00000802)),
          "debug FSR recognized with access bit");
    check(!vd_armv7_fsr_is_debug_event(UINT32_C(0x00000000)),
          "zero FSR is not debug");
    check(!vd_armv7_fsr_is_debug_event(UINT32_C(0x00000402)),
          "status 0x12 is not debug");
    check(!vd_armv7_fsr_is_debug_event(UINT32_C(0xffffffff)),
          "all-one FSR is not debug");
}

static void test_range(void)
{
    check(vd_armv7_range_contains(UINT32_C(0x1000), 4,
                                   UINT32_C(0x1000)),
          "range includes first byte");
    check(vd_armv7_range_contains(UINT32_C(0x1000), 4,
                                   UINT32_C(0x1003)),
          "range includes last byte");
    check(!vd_armv7_range_contains(UINT32_C(0x1000), 4,
                                    UINT32_C(0x1004)),
          "range excludes end");
    check(!vd_armv7_range_contains(UINT32_C(0x1000), 4,
                                    UINT32_C(0x0fff)),
          "range excludes lower address");
    check(!vd_armv7_range_contains(UINT32_C(0x1000), 0,
                                    UINT32_C(0x1000)),
          "empty range never matches");

    check(vd_armv7_range_contains(UINT32_C(0xfffffffe), 4,
                                   UINT32_C(0xfffffffe)),
          "overflowing range includes high first byte");
    check(vd_armv7_range_contains(UINT32_C(0xfffffffe), 4,
                                   UINT32_C(0xffffffff)),
          "overflowing range includes high last address");
    check(!vd_armv7_range_contains(UINT32_C(0xfffffffe), 4,
                                    UINT32_C(0x00000000)),
          "overflowing range does not wrap to zero");
}

int main(void)
{
    test_exec();
    test_context();
    test_watch();
    test_fsr();
    test_range();

    if(failures)
        return 1;
    puts("PASS: Cortex-A9 hardware debug encoding and validation");
    return 0;
}
