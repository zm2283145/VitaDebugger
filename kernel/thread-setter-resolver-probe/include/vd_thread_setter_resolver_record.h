#ifndef VD_THREAD_SETTER_RESOLVER_RECORD_H
#define VD_THREAD_SETTER_RESOLVER_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define VD_THREAD_SETTER_RESOLVER_MAGIC UINT32_C(0x56545352)
#define VD_THREAD_SETTER_RESOLVER_VERSION UINT32_C(1)
#define VD_THREAD_SETTER_RESOLVER_RECORD_SIZE UINT32_C(640)
#define VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT UINT32_C(4)
#define VD_THREAD_SETTER_RESOLVER_TARGET_COUNT UINT32_C(4)
#define VD_THREAD_SETTER_RESOLVER_CODE_BYTES UINT32_C(64)
#define VD_THREAD_SETTER_RESOLVER_EXPORT_MAX_BYTES UINT32_C(65536)
#define VD_THREAD_SETTER_RESOLVER_ANY_LIBRARY UINT32_C(0xFFFFFFFF)

#define VD_THREAD_SETTER_RESOLVER_MODULE "SceKernelThreadMgr"
#define VD_THREAD_SETTER_RESOLVER_SKPRX_PATH \
    "ux0:app/VDCP00008/module/vd-thread-setter-resolver.skprx"
#define VD_THREAD_SETTER_RESOLVER_RECORD_A_PATH \
    "ux0:data/VitaDebugger/thread-setter-resolver-v1-a.bin"
#define VD_THREAD_SETTER_RESOLVER_RECORD_B_PATH \
    "ux0:data/VitaDebugger/thread-setter-resolver-v1-b.bin"

#define VD_THREAD_SETTER_NID_CORE_GET UINT32_C(0x5022689D)
#define VD_THREAD_SETTER_NID_CORE_SET UINT32_C(0x64E89DE9)
#define VD_THREAD_SETTER_NID_VFP_GET UINT32_C(0x5CDE387A)
#define VD_THREAD_SETTER_NID_VFP_SET UINT32_C(0x49A0B679)

#define VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED \
    ((int32_t)(-INT32_C(2147483647) - INT32_C(1)))
#define VD_THREAD_SETTER_RESOLVER_EMPTY_ADDRESS INT32_C(-200)
#define VD_THREAD_SETTER_RESOLVER_EXECUTE_PERMISSION UINT32_C(1)

#define VD_THREAD_SETTER_RESOLVER_JOURNAL_EMPTY (-1)
#define VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT (-2)
#define VD_THREAD_SETTER_RESOLVER_JOURNAL_CONFLICT (-3)

enum vd_thread_setter_resolver_state {
    VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED = 1,
    VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED = 2,
    VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE = 3,
};

enum vd_thread_setter_resolver_result {
    VD_THREAD_SETTER_RESOLVER_OK = 0,
    VD_THREAD_SETTER_RESOLVER_NOT_RUN = -100,
    VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_LOOKUP = -1,
    VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_INFO = -2,
    VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_NAME = -3,
    VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_RANGE = -4,
    VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_MISSING = -5,
    VD_THREAD_SETTER_RESOLVER_ERROR_ADDRESS_RANGE = -6,
    VD_THREAD_SETTER_RESOLVER_ERROR_NOT_EXECUTABLE = -7,
    VD_THREAD_SETTER_RESOLVER_ERROR_CODE_WINDOW = -8,
};

enum vd_thread_setter_resolver_kind {
    VD_THREAD_SETTER_RESOLVER_CORE_GET = 1,
    VD_THREAD_SETTER_RESOLVER_CORE_SET = 2,
    VD_THREAD_SETTER_RESOLVER_VFP_GET = 3,
    VD_THREAD_SETTER_RESOLVER_VFP_SET = 4,
};

enum vd_thread_setter_resolver_record_flag {
    VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK = UINT32_C(1) << 0,
    VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK = UINT32_C(1) << 1,
    VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK = UINT32_C(1) << 2,
    VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK = UINT32_C(1) << 3,
    VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED = UINT32_C(1) << 4,
    VD_THREAD_SETTER_RESOLVER_FLAG_ALL_RESOLVED = UINT32_C(1) << 5,
    VD_THREAD_SETTER_RESOLVER_FLAG_ALL_EXECUTABLE = UINT32_C(1) << 6,
    VD_THREAD_SETTER_RESOLVER_FLAG_ALL_CAPTURED = UINT32_C(1) << 7,
    VD_THREAD_SETTER_RESOLVER_FLAG_COMPLETE = UINT32_C(1) << 8,
};

#define VD_THREAD_SETTER_RESOLVER_RECORD_FLAGS_ALLOWED \
    (VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK | \
     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK | \
     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK | \
     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK | \
     VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED | \
     VD_THREAD_SETTER_RESOLVER_FLAG_ALL_RESOLVED | \
     VD_THREAD_SETTER_RESOLVER_FLAG_ALL_EXECUTABLE | \
     VD_THREAD_SETTER_RESOLVER_FLAG_ALL_CAPTURED | \
     VD_THREAD_SETTER_RESOLVER_FLAG_COMPLETE)

enum vd_thread_setter_resolver_target_flag {
    VD_THREAD_SETTER_TARGET_FLAG_RESOLVED = UINT32_C(1) << 0,
    VD_THREAD_SETTER_TARGET_FLAG_THUMB = UINT32_C(1) << 1,
    VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT = UINT32_C(1) << 2,
    VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE = UINT32_C(1) << 3,
    VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED = UINT32_C(1) << 4,
    VD_THREAD_SETTER_TARGET_FLAG_CAPTURED = UINT32_C(1) << 5,
};

#define VD_THREAD_SETTER_TARGET_FLAGS_ALLOWED \
    (VD_THREAD_SETTER_TARGET_FLAG_RESOLVED | \
     VD_THREAD_SETTER_TARGET_FLAG_THUMB | \
     VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT | \
     VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE | \
     VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED | \
     VD_THREAD_SETTER_TARGET_FLAG_CAPTURED)

struct vd_thread_setter_resolver_segment {
    uint32_t base;
    uint32_t memsz;
    uint32_t filesz;
    uint32_t permissions;
};

struct vd_thread_setter_resolver_target {
    uint32_t kind;
    uint32_t nid;
    int32_t lookup_result;
    uint32_t flags;
    uint32_t raw_address;
    uint32_t code_address;
    int32_t segment_index;
    uint32_t segment_offset;
    uint32_t code_size;
    uint8_t code[VD_THREAD_SETTER_RESOLVER_CODE_BYTES];
};

/*
 * This fixed record is deliberately the probe's entire disclosure surface:
 * four fixed NIDs, four module segment descriptors, and at most 64 bytes from
 * each resolved function after its address has been proven to lie wholly in an
 * executable SceKernelThreadMgr segment. No address or NID comes from user
 * input. Two alternating copies form a small lifecycle journal.
 */
struct vd_thread_setter_resolver_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t sequence;
    uint32_t state;
    int32_t result;
    uint32_t flags;
    int32_t firmware_result;
    uint32_t firmware_version;
    int32_t module_lookup_result;
    int32_t module_info_result;
    int32_t module_id;
    uint32_t module_nid;
    uint32_t exports_start;
    uint32_t exports_end;
    char module_name[28];
    uint32_t segment_count;
    uint32_t resolved_count;
    uint32_t executable_count;
    uint32_t captured_count;
    uint32_t lookup_library_nid;
    uint32_t reserved_header[3];
    struct vd_thread_setter_resolver_segment
        segments[VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT];
    struct vd_thread_setter_resolver_target
        targets[VD_THREAD_SETTER_RESOLVER_TARGET_COUNT];
    uint32_t reserved[12];
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_thread_setter_resolver_segment) == 16,
              "resolver segment ABI changed");
static_assert(sizeof(struct vd_thread_setter_resolver_target) == 100,
              "resolver target ABI changed");
static_assert(sizeof(struct vd_thread_setter_resolver_record) ==
                  VD_THREAD_SETTER_RESOLVER_RECORD_SIZE,
              "resolver record ABI changed");
#else
_Static_assert(sizeof(struct vd_thread_setter_resolver_segment) == 16,
               "resolver segment ABI changed");
_Static_assert(sizeof(struct vd_thread_setter_resolver_target) == 100,
               "resolver target ABI changed");
_Static_assert(sizeof(struct vd_thread_setter_resolver_record) ==
                   VD_THREAD_SETTER_RESOLVER_RECORD_SIZE,
               "resolver record ABI changed");
#endif

static inline uint32_t vd_thread_setter_resolver_expected_kind(uint32_t index)
{
    static const uint32_t kinds[VD_THREAD_SETTER_RESOLVER_TARGET_COUNT] = {
        VD_THREAD_SETTER_RESOLVER_CORE_GET,
        VD_THREAD_SETTER_RESOLVER_CORE_SET,
        VD_THREAD_SETTER_RESOLVER_VFP_GET,
        VD_THREAD_SETTER_RESOLVER_VFP_SET,
    };
    return index < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT ? kinds[index] : 0;
}

static inline uint32_t vd_thread_setter_resolver_expected_nid(uint32_t index)
{
    static const uint32_t nids[VD_THREAD_SETTER_RESOLVER_TARGET_COUNT] = {
        VD_THREAD_SETTER_NID_CORE_GET,
        VD_THREAD_SETTER_NID_CORE_SET,
        VD_THREAD_SETTER_NID_VFP_GET,
        VD_THREAD_SETTER_NID_VFP_SET,
    };
    return index < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT ? nids[index] : 0;
}

static inline int vd_thread_setter_resolver_bytes_zero(const void* data,
                                                        size_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    size_t i;
    for(i = 0; i < size; ++i)
    {
        if(bytes[i] != 0)
            return 0;
    }
    return 1;
}

static inline int vd_thread_setter_resolver_range_within(uint32_t start,
                                                          uint32_t size,
                                                          uint32_t base,
                                                          uint32_t span)
{
    if(size == 0 || span == 0 || start < base)
        return 0;
    const uint32_t offset = start - base;
    return offset < span && size <= span - offset;
}

static inline uint32_t vd_thread_setter_resolver_checksum(
    const struct vd_thread_setter_resolver_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_thread_setter_resolver_record, checksum);
    const size_t checksum_end = checksum_start + sizeof(record->checksum);
    uint32_t hash = UINT32_C(2166136261);
    size_t i;

    for(i = 0; i < sizeof(*record); ++i)
    {
        const uint8_t value =
            (i >= checksum_start && i < checksum_end) ? 0 : bytes[i];
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static inline int vd_thread_setter_resolver_header_valid(
    const struct vd_thread_setter_resolver_record* record)
{
    return record &&
           record->magic == VD_THREAD_SETTER_RESOLVER_MAGIC &&
           record->version == VD_THREAD_SETTER_RESOLVER_VERSION &&
           record->size == (uint32_t)sizeof(*record) &&
           record->checksum == vd_thread_setter_resolver_checksum(record);
}

static inline int vd_thread_setter_resolver_revision_newer(uint32_t left,
                                                            uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

/* Presence and validity are deliberately separate. A valid record must never
 * hide a torn, corrupt, or unreadable sibling because the sibling may be the
 * newer lifecycle transition that needs to be preserved for diagnosis. */
static inline int vd_thread_setter_resolver_select_latest(
    int present_a, int valid_a, uint32_t revision_a,
    int present_b, int valid_b, uint32_t revision_b)
{
    if(!present_a && !present_b)
        return VD_THREAD_SETTER_RESOLVER_JOURNAL_EMPTY;
    if((present_a && !valid_a) || (present_b && !valid_b))
        return VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
    if(valid_a && valid_b && revision_a == revision_b)
        return VD_THREAD_SETTER_RESOLVER_JOURNAL_CONFLICT;
    if(valid_b && (!valid_a ||
                   vd_thread_setter_resolver_revision_newer(revision_b,
                                                            revision_a)))
        return 1;
    return valid_a ? 0 : VD_THREAD_SETTER_RESOLVER_JOURNAL_CORRUPT;
}

static inline void vd_thread_setter_resolver_init_attempt(
    struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    size_t byte_index;
    uint8_t* bytes = (uint8_t*)record;
    for(byte_index = 0; byte_index < sizeof(*record); ++byte_index)
        bytes[byte_index] = 0;
    record->state = VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED;
    record->result = VD_THREAD_SETTER_RESOLVER_NOT_RUN;
    record->firmware_result = VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED;
    record->module_lookup_result = VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED;
    record->module_info_result = VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED;
    record->lookup_library_nid = VD_THREAD_SETTER_RESOLVER_ANY_LIBRARY;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        record->targets[i].kind =
            vd_thread_setter_resolver_expected_kind(i);
        record->targets[i].nid =
            vd_thread_setter_resolver_expected_nid(i);
        record->targets[i].lookup_result =
            VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED;
        record->targets[i].segment_index = -1;
    }
}

static inline int vd_thread_setter_resolver_targets_fixed(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        if(record->targets[i].kind !=
               vd_thread_setter_resolver_expected_kind(i) ||
           record->targets[i].nid !=
               vd_thread_setter_resolver_expected_nid(i))
            return 0;
    }
    return 1;
}

static inline int vd_thread_setter_resolver_attempt_payload_valid(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    if((record->state != VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED &&
        record->state != VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED) ||
       record->result != VD_THREAD_SETTER_RESOLVER_NOT_RUN ||
       record->flags != 0 ||
       record->firmware_result != VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
       record->firmware_version != 0 ||
       record->module_lookup_result !=
           VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
       record->module_info_result !=
           VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
       record->module_id != 0 || record->module_nid != 0 ||
       record->exports_start != 0 || record->exports_end != 0 ||
       !vd_thread_setter_resolver_bytes_zero(record->module_name,
                                              sizeof(record->module_name)) ||
       record->segment_count != 0 || record->resolved_count != 0 ||
       record->executable_count != 0 || record->captured_count != 0 ||
       record->lookup_library_nid !=
           VD_THREAD_SETTER_RESOLVER_ANY_LIBRARY ||
       !vd_thread_setter_resolver_bytes_zero(record->reserved_header,
                                              sizeof(record->reserved_header)) ||
       !vd_thread_setter_resolver_bytes_zero(record->segments,
                                              sizeof(record->segments)) ||
       !vd_thread_setter_resolver_bytes_zero(record->reserved,
                                              sizeof(record->reserved)) ||
       !vd_thread_setter_resolver_targets_fixed(record))
        return 0;

    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_target* target =
            &record->targets[i];
        if(target->lookup_result !=
               VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
           target->flags != 0 || target->raw_address != 0 ||
           target->code_address != 0 || target->segment_index != -1 ||
           target->segment_offset != 0 || target->code_size != 0 ||
           !vd_thread_setter_resolver_bytes_zero(target->code,
                                                  sizeof(target->code)))
            return 0;
    }
    return 1;
}

static inline int vd_thread_setter_resolver_name_is_expected(
    const char module_name[28])
{
    static const char expected[] = VD_THREAD_SETTER_RESOLVER_MODULE;
    size_t i;
    for(i = 0; i < sizeof(expected); ++i)
    {
        if(module_name[i] != expected[i])
            return 0;
    }
    for(; i < 28; ++i)
    {
        if(module_name[i] != '\0')
            return 0;
    }
    return 1;
}

static inline int vd_thread_setter_resolver_exports_bounded(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    const uint32_t start = record->exports_start;
    const uint32_t end = record->exports_end;
    if(start == 0 || end <= start ||
       end - start > VD_THREAD_SETTER_RESOLVER_EXPORT_MAX_BYTES)
        return 0;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_segment* segment =
            &record->segments[i];
        if(vd_thread_setter_resolver_range_within(
               start, end - start, segment->base, segment->memsz))
            return 1;
    }
    return 0;
}

static inline int vd_thread_setter_resolver_expected_result(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    if(record->module_lookup_result < 0)
        return VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_LOOKUP;
    if(record->module_info_result < 0)
        return VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_INFO;
    if(!vd_thread_setter_resolver_name_is_expected(record->module_name))
        return VD_THREAD_SETTER_RESOLVER_ERROR_MODULE_NAME;
    if(!vd_thread_setter_resolver_exports_bounded(record))
        return VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_RANGE;
    if(record->resolved_count != VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        return VD_THREAD_SETTER_RESOLVER_ERROR_EXPORT_MISSING;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        if((record->targets[i].flags &
            VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT) == 0)
            return VD_THREAD_SETTER_RESOLVER_ERROR_ADDRESS_RANGE;
    }
    if(record->executable_count != VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        return VD_THREAD_SETTER_RESOLVER_ERROR_NOT_EXECUTABLE;
    if(record->captured_count != VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        return VD_THREAD_SETTER_RESOLVER_ERROR_CODE_WINDOW;
    return VD_THREAD_SETTER_RESOLVER_OK;
}

static inline void vd_thread_setter_resolver_finalize(
    struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    record->resolved_count = 0;
    record->executable_count = 0;
    record->captured_count = 0;
    record->flags &= VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK |
                     VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED;
    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        const uint32_t flags = record->targets[i].flags;
        if((flags & VD_THREAD_SETTER_TARGET_FLAG_RESOLVED) != 0)
            record->resolved_count++;
        if((flags & (VD_THREAD_SETTER_TARGET_FLAG_RESOLVED |
                     VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT |
                     VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE)) ==
            (VD_THREAD_SETTER_TARGET_FLAG_RESOLVED |
             VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT |
             VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE))
            record->executable_count++;
        if((flags & VD_THREAD_SETTER_TARGET_FLAG_CAPTURED) != 0)
            record->captured_count++;
    }
    if(record->resolved_count == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_ALL_RESOLVED;
    if(record->executable_count == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_ALL_EXECUTABLE;
    if(record->captured_count == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)
        record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_ALL_CAPTURED;
    record->flags |= VD_THREAD_SETTER_RESOLVER_FLAG_COMPLETE;
    record->result = vd_thread_setter_resolver_expected_result(record);
    record->state = VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE;
}

static inline int vd_thread_setter_resolver_complete_payload_valid(
    const struct vd_thread_setter_resolver_record* record)
{
    uint32_t i;
    uint32_t active_segments = 0;
    uint32_t resolved = 0;
    uint32_t executable = 0;
    uint32_t captured = 0;
    if(record->state != VD_THREAD_SETTER_RESOLVER_STATE_COMPLETE ||
       (record->flags & ~VD_THREAD_SETTER_RESOLVER_RECORD_FLAGS_ALLOWED) != 0 ||
       (record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_COMPLETE) == 0 ||
       record->lookup_library_nid !=
           VD_THREAD_SETTER_RESOLVER_ANY_LIBRARY ||
       !vd_thread_setter_resolver_bytes_zero(record->reserved_header,
                                              sizeof(record->reserved_header)) ||
       !vd_thread_setter_resolver_bytes_zero(record->reserved,
                                              sizeof(record->reserved)) ||
       !vd_thread_setter_resolver_targets_fixed(record))
        return 0;

    /* The reported version is useful evidence, but is not a firmware gate:
     * Enso_ex can deliberately spoof this field. It must only agree with the
     * success/failure state of the read-only metadata query. */
    if(record->firmware_result == VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
       record->module_lookup_result ==
           VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
       (record->firmware_result >= 0 && record->firmware_version == 0) ||
       (record->firmware_result < 0 && record->firmware_version != 0))
        return 0;

    if(record->module_lookup_result < 0)
    {
        if(record->module_info_result !=
               VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
           record->module_id != 0 || record->module_nid != 0 ||
           record->exports_start != 0 || record->exports_end != 0 ||
           !vd_thread_setter_resolver_bytes_zero(record->module_name,
                                                  sizeof(record->module_name)))
            return 0;
    }
    else if(record->module_info_result ==
                VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED ||
            record->module_id == 0 || record->module_nid == 0 ||
            record->exports_start == 0 || record->exports_end == 0)
    {
        return 0;
    }

    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_segment* segment =
            &record->segments[i];
        if(segment->base == 0 && segment->memsz == 0 &&
           segment->filesz == 0 && segment->permissions == 0)
            continue;
        if(segment->base == 0 || segment->memsz == 0 ||
           segment->filesz > segment->memsz ||
           segment->memsz > UINT32_MAX - segment->base)
            return 0;
        active_segments++;
    }
    if(active_segments != record->segment_count)
        return 0;
    if(record->module_info_result < 0)
    {
        if(active_segments != 0)
            return 0;
    }
    else if(record->module_lookup_result < 0 || active_segments == 0)
    {
        return 0;
    }

    for(i = 0; i < VD_THREAD_SETTER_RESOLVER_TARGET_COUNT; ++i)
    {
        const struct vd_thread_setter_resolver_target* target =
            &record->targets[i];
        const uint32_t flags = target->flags;
        const uint32_t discovery_flags =
            VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK |
            VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK |
            VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK |
            VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED;
        const int lookup_was_expected =
            (record->flags & discovery_flags) == discovery_flags;
        if((flags & ~VD_THREAD_SETTER_TARGET_FLAGS_ALLOWED) != 0)
            return 0;
        if((target->lookup_result !=
                VD_THREAD_SETTER_RESOLVER_NOT_ATTEMPTED) !=
           lookup_was_expected)
            return 0;
        if(target->lookup_result < 0)
        {
            if(flags != 0 || target->raw_address != 0 ||
               target->code_address != 0 || target->segment_index != -1 ||
               target->segment_offset != 0 || target->code_size != 0 ||
               !vd_thread_setter_resolver_bytes_zero(target->code,
                                                      sizeof(target->code)))
                return 0;
            continue;
        }
        if((flags & VD_THREAD_SETTER_TARGET_FLAG_RESOLVED) == 0 ||
           target->raw_address == 0 || target->code_address == 0 ||
           target->code_address != (target->raw_address & ~UINT32_C(1)) ||
           (((target->raw_address & UINT32_C(1)) != 0) !=
            ((flags & VD_THREAD_SETTER_TARGET_FLAG_THUMB) != 0)))
            return 0;
        resolved++;

        if((flags & VD_THREAD_SETTER_TARGET_FLAG_IN_SEGMENT) == 0)
        {
            if(target->segment_index != -1 || target->segment_offset != 0 ||
               (flags & (VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE |
                         VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED |
                         VD_THREAD_SETTER_TARGET_FLAG_CAPTURED)) != 0 ||
               target->code_size != 0 ||
               !vd_thread_setter_resolver_bytes_zero(target->code,
                                                      sizeof(target->code)))
                return 0;
            continue;
        }
        if(target->segment_index < 0 ||
           target->segment_index >=
               (int32_t)VD_THREAD_SETTER_RESOLVER_SEGMENT_COUNT)
            return 0;
        const struct vd_thread_setter_resolver_segment* segment =
            &record->segments[target->segment_index];
        if(target->segment_offset >= segment->memsz ||
           target->code_address != segment->base + target->segment_offset)
            return 0;
        if(((segment->permissions &
             VD_THREAD_SETTER_RESOLVER_EXECUTE_PERMISSION) != 0) !=
           ((flags & VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE) != 0))
            return 0;
        if((flags & VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE) != 0)
            executable++;
        const int expected_window =
            (flags & VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE) != 0 &&
            vd_thread_setter_resolver_range_within(
                target->code_address,
                VD_THREAD_SETTER_RESOLVER_CODE_BYTES,
                segment->base, segment->memsz);
        if(((flags & VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED) != 0) !=
           expected_window)
            return 0;
        if((flags & VD_THREAD_SETTER_TARGET_FLAG_CAPTURED) != 0)
        {
            if((flags & (VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE |
                         VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED)) !=
               (VD_THREAD_SETTER_TARGET_FLAG_EXECUTABLE |
                VD_THREAD_SETTER_TARGET_FLAG_WINDOW_BOUNDED) ||
               target->code_size != VD_THREAD_SETTER_RESOLVER_CODE_BYTES)
                return 0;
            captured++;
        }
        else if(target->code_size != 0 ||
                !vd_thread_setter_resolver_bytes_zero(target->code,
                                                       sizeof(target->code)))
        {
            return 0;
        }
    }

    if(record->resolved_count != resolved ||
       record->executable_count != executable ||
       record->captured_count != captured ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_ALL_RESOLVED) != 0) !=
        (resolved == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_ALL_EXECUTABLE) != 0) !=
        (executable == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_ALL_CAPTURED) != 0) !=
        (captured == VD_THREAD_SETTER_RESOLVER_TARGET_COUNT)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_FIRMWARE_QUERY_OK) != 0) !=
        (record->firmware_result >= 0)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_LOOKUP_OK) != 0) !=
        (record->module_lookup_result >= 0)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_INFO_OK) != 0) !=
        (record->module_info_result >= 0)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_MODULE_NAME_OK) != 0) !=
        vd_thread_setter_resolver_name_is_expected(record->module_name)) ||
       (((record->flags & VD_THREAD_SETTER_RESOLVER_FLAG_EXPORTS_BOUNDED) != 0) !=
        vd_thread_setter_resolver_exports_bounded(record)) ||
       record->result != vd_thread_setter_resolver_expected_result(record))
        return 0;
    return 1;
}

static inline int vd_thread_setter_resolver_record_valid(
    const struct vd_thread_setter_resolver_record* record)
{
    if(!vd_thread_setter_resolver_header_valid(record))
        return 0;
    if(record->state == VD_THREAD_SETTER_RESOLVER_STATE_ATTEMPTED ||
       record->state == VD_THREAD_SETTER_RESOLVER_STATE_KERNEL_ENTERED)
        return vd_thread_setter_resolver_attempt_payload_valid(record);
    return vd_thread_setter_resolver_complete_payload_valid(record);
}

#endif
