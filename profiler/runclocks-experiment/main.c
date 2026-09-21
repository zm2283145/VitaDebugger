#include "vitaprofiler.h"
#include "vitaprofiler_stream.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#ifndef VP_RUNCLOCKS_EXPERIMENT_ENABLED
#define VP_RUNCLOCKS_EXPERIMENT_ENABLED 0
#endif
#ifndef VP_RUNCLOCKS_EXPERIMENT_ID
#define VP_RUNCLOCKS_EXPERIMENT_ID "disabled"
#endif
#ifndef VP_RUNCLOCKS_DEVICE_MODEL
#define VP_RUNCLOCKS_DEVICE_MODEL "unknown"
#endif
#ifndef VP_RUNCLOCKS_DEVICE_ID
#define VP_RUNCLOCKS_DEVICE_ID "unknown"
#endif
#ifndef VP_RUNCLOCKS_FIRMWARE
#define VP_RUNCLOCKS_FIRMWARE "unknown"
#endif
#ifndef VP_RUNCLOCKS_BUILD_ID
#define VP_RUNCLOCKS_BUILD_ID "unknown"
#endif
#ifndef VP_RUNCLOCKS_CLOCK_PROFILE
#define VP_RUNCLOCKS_CLOCK_PROFILE "unknown"
#endif
#ifndef VP_RUNCLOCKS_POWER_STATE
#define VP_RUNCLOCKS_POWER_STATE "unknown"
#endif

#if VP_RUNCLOCKS_EXPERIMENT_ENABLED != 0 && \
    VP_RUNCLOCKS_EXPERIMENT_ENABLED != 1
#error "VP_RUNCLOCKS_EXPERIMENT_ENABLED must be zero or one"
#endif

#define RC_TITLE_ID "VDPR00001"
#define RC_OUTPUT_DIRECTORY "ux0:data/VitaDebugger"
#define RC_SAMPLE_INTERVAL_US UINT32_C(25000)
#define RC_CAPTURE_LIMIT_US UINT32_C(5000000)
#define RC_SAMPLE_LIMIT 80u
#define RC_RING_CAPACITY 64u
#define RC_DICTIONARY_BYTES 4096u
#define RC_MAX_PHASES 5u
#define RC_MAX_WORKERS 2u
#define RC_MAX_GENERATIONS 6u
#define RC_CONFIRM_TIMEOUT_US UINT64_C(30000000)
#define RC_READY_TIMEOUT_US UINT64_C(1000000)
#define RC_WORK_SINK_WORDS 1024u

#if VP_RUNCLOCKS_EXPERIMENT_ENABLED

enum rc_work_mode {
    RC_WORK_SLEEP = 0,
    RC_WORK_BUSY = 1,
};

struct rc_phase_spec {
    const char* phase_id;
    const char* condition;
    uint32_t repeat;
    uint32_t duration_us;
    uint32_t worker_count;
    enum rc_work_mode mode;
};

struct rc_worker {
    SceUID thread_id;
    uint32_t generation;
    uint32_t slot;
    uint32_t duration_us;
    enum rc_work_mode mode;
    volatile uint32_t ready;
    volatile uint32_t start;
    volatile uint32_t finished;
    volatile uint32_t release;
};

struct rc_generation_record {
    uint32_t thread_id;
    uint32_t generation;
    char label[32];
    uint32_t first_event_index;
    uint32_t last_event_index;
};

struct rc_phase_record {
    const struct rc_phase_spec* spec;
    uint32_t first_event_index;
    uint32_t last_event_index;
};

static const struct rc_phase_spec rc_phase_plan[RC_MAX_PHASES] = {
    {"sleep-200ms-r1", "worker repeatedly calls sceKernelDelayThread", 1u,
     200000u, 1u, RC_WORK_SLEEP},
    {"busy-200ms-r1", "bounded volatile integer workload", 1u,
     200000u, 1u, RC_WORK_BUSY},
    {"sleep-400ms-r2", "worker repeatedly calls sceKernelDelayThread", 2u,
     400000u, 1u, RC_WORK_SLEEP},
    {"busy-400ms-r2", "bounded volatile integer workload", 2u,
     400000u, 1u, RC_WORK_BUSY},
    {"busy-400ms-two-workers", "two bounded volatile integer workloads", 1u,
     400000u, 2u, RC_WORK_BUSY},
};

static volatile uint32_t rc_work_sink[RC_WORK_SINK_WORDS];
static struct rc_worker rc_workers[RC_MAX_WORKERS];
static struct rc_generation_record rc_generations[RC_MAX_GENERATIONS];
static struct rc_phase_record rc_phases[RC_MAX_PHASES];
static struct vp_slot rc_slots[RC_RING_CAPACITY];
static struct vp_context rc_context;
static struct vp_name_dictionary rc_names;
static struct vp_name_entry rc_name_entries[1];
static char rc_name_text[1];
static uint8_t rc_dictionary_buffer[RC_DICTIONARY_BYTES];
static struct vp_stream_writer_v2 rc_writer;
static SceUID rc_capture_fd = -1;
static uint32_t rc_generation_count;
static uint32_t rc_active_worker_count;
static char rc_captured_at_utc[32];

static int rc_write_all(SceUID fd, const void* data, size_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    size_t offset = 0u;
    while (offset < size) {
        const size_t remaining = size - offset;
        const unsigned int chunk =
            remaining > UINT32_MAX ? UINT32_MAX : (unsigned int)remaining;
        const int written = sceIoWrite(fd, bytes + offset, chunk);
        if (written <= 0)
            return written < 0 ? written : -1;
        offset += (size_t)written;
    }
    return 0;
}

static int rc_file_sink(void* user, const uint8_t* data, size_t size)
{
    const SceUID fd = *(const SceUID*)user;
    return rc_write_all(fd, data, size);
}

static int rc_write_text(SceUID fd, const char* text)
{
    return rc_write_all(fd, text, strlen(text));
}

static int rc_write_format(SceUID fd, const char* format, ...)
{
    char buffer[256];
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(buffer))
        return -1;
    return rc_write_all(fd, buffer, (size_t)length);
}

static int rc_write_json_string(SceUID fd, const char* value)
{
    const uint8_t* cursor = (const uint8_t*)value;
    if (rc_write_text(fd, "\"") < 0)
        return -1;
    while (*cursor != 0u) {
        char escape[7];
        const uint8_t character = *cursor++;
        if (character == '"' || character == '\\') {
            escape[0] = '\\';
            escape[1] = (char)character;
            if (rc_write_all(fd, escape, 2u) < 0)
                return -1;
        } else if (character < 0x20u) {
            const int length = snprintf(
                escape, sizeof(escape), "\\u%04x", (unsigned int)character);
            if (length != 6 || rc_write_all(fd, escape, 6u) < 0)
                return -1;
        } else if (rc_write_all(fd, &character, 1u) < 0) {
            return -1;
        }
    }
    return rc_write_text(fd, "\"");
}

static int rc_identifier_is_safe(const char* value)
{
    size_t length = 0u;
    while (value[length] != '\0') {
        const char character = value[length];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_'))
            return 0;
        ++length;
    }
    return length >= 1u && length <= 48u;
}

static int rc_metadata_text_is_safe(const char* value)
{
    size_t length = 0u;
    while (value[length] != '\0') {
        const uint8_t character = (uint8_t)value[length];
        if (character < 0x20u || character == 0x7fu)
            return 0;
        ++length;
    }
    return length >= 1u && length <= 160u;
}

static int rc_metadata_is_configured(void)
{
    return rc_identifier_is_safe(VP_RUNCLOCKS_EXPERIMENT_ID) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_DEVICE_MODEL) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_DEVICE_ID) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_FIRMWARE) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_BUILD_ID) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_CLOCK_PROFILE) &&
           rc_metadata_text_is_safe(VP_RUNCLOCKS_POWER_STATE) &&
           strcmp(VP_RUNCLOCKS_DEVICE_MODEL, "unknown") != 0 &&
           strcmp(VP_RUNCLOCKS_DEVICE_ID, "unknown") != 0 &&
           strcmp(VP_RUNCLOCKS_FIRMWARE, "unknown") != 0 &&
           strcmp(VP_RUNCLOCKS_BUILD_ID, "unknown") != 0 &&
           strcmp(VP_RUNCLOCKS_CLOCK_PROFILE, "unknown") != 0 &&
           strcmp(VP_RUNCLOCKS_POWER_STATE, "unknown") != 0;
}

static int rc_wait_for_confirmation(void)
{
    const uint64_t deadline =
        (uint64_t)sceKernelGetProcessTimeWide() + RC_CONFIRM_TIMEOUT_US;
    uint32_t previous = 0u;
    while ((uint64_t)sceKernelGetProcessTimeWide() < deadline) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            const uint32_t pressed = pad.buttons & ~previous;
            previous = pad.buttons;
            if ((pressed & SCE_CTRL_CROSS) != 0u)
                return 1;
            if ((pressed & SCE_CTRL_CIRCLE) != 0u)
                return 0;
        }
        sceKernelDelayThread(10000);
    }
    return 0;
}

static int rc_worker_main(SceSize args, void* argp)
{
    uint32_t index;
    struct rc_worker* worker;
    uint64_t deadline;
    uint32_t state;
    if (args != sizeof(index) || argp == NULL)
        return -1;
    memcpy(&index, argp, sizeof(index));
    if (index >= RC_MAX_WORKERS)
        return -1;
    worker = &rc_workers[index];
    __atomic_store_n(&worker->ready, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&worker->start, __ATOMIC_ACQUIRE) == 0u)
        sceKernelDelayThread(100);
    deadline = (uint64_t)sceKernelGetProcessTimeWide() + worker->duration_us;
    state = UINT32_C(0x9e3779b9) ^ worker->generation;
    while ((uint64_t)sceKernelGetProcessTimeWide() < deadline) {
        if (worker->mode == RC_WORK_SLEEP) {
            sceKernelDelayThread(1000);
        } else {
            uint32_t index2;
            for (index2 = 0u; index2 < RC_WORK_SINK_WORDS; ++index2) {
                state ^= state << 13u;
                state ^= state >> 17u;
                state ^= state << 5u;
                state += index2;
            }
            rc_work_sink[worker->slot] ^= state;
        }
    }
    __atomic_store_n(&worker->finished, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&worker->release, __ATOMIC_ACQUIRE) == 0u)
        sceKernelDelayThread(1000);
    return (int)(state & 0x7fffffffu);
}

static int rc_drain(void)
{
    size_t drained;
    int result;
    do {
        drained = 0u;
        result = vp_stream_writer_drain_v2(
            &rc_writer, RC_RING_CAPACITY, &drained);
        if (result != VP_RESULT_OK)
            return result;
    } while (drained != 0u);
    return VP_RESULT_OK;
}

static int rc_start_phase_workers(
    const struct rc_phase_spec* phase, uint32_t phase_index)
{
    uint32_t worker_index;
    const uint64_t ready_deadline =
        (uint64_t)sceKernelGetProcessTimeWide() + RC_READY_TIMEOUT_US;
    rc_active_worker_count = 0u;
    for (worker_index = 0u; worker_index < phase->worker_count;
         ++worker_index) {
        struct rc_worker* worker = &rc_workers[worker_index];
        struct rc_generation_record* generation;
        struct vp_stream_v2_thread metadata;
        SceUID thread;
        uint32_t argument = worker_index;
        if (rc_generation_count >= RC_MAX_GENERATIONS)
            return VP_ERROR_CAPACITY;
        memset(worker, 0, sizeof(*worker));
        worker->generation = rc_generation_count;
        worker->slot = worker_index;
        worker->duration_us = phase->duration_us;
        worker->mode = phase->mode;
        thread = sceKernelCreateThread(
            "vp runClocks worker", rc_worker_main, 0x10000100,
            32u * 1024u, 0, 0, NULL);
        if (thread < 0)
            return VP_ERROR_PLATFORM;
        worker->thread_id = thread;
        generation = &rc_generations[rc_generation_count];
        memset(generation, 0, sizeof(*generation));
        generation->thread_id = (uint32_t)thread;
        generation->generation = rc_generation_count;
        if (snprintf(generation->label, sizeof(generation->label),
                     "phase-%u-worker-%u", phase_index, worker_index) < 0) {
            sceKernelDeleteThread(thread);
            return VP_ERROR_STATE;
        }
        generation->first_event_index = UINT32_MAX;
        if (sceKernelStartThread(thread, sizeof(argument), &argument) < 0) {
            sceKernelDeleteThread(thread);
            return VP_ERROR_PLATFORM;
        }
        ++rc_active_worker_count;
        memset(&metadata, 0, sizeof(metadata));
        metadata.identity =
            (UINT64_C(0x52434c4b00000000) | (rc_generation_count + 1u));
        metadata.thread_id = (uint32_t)thread;
        metadata.generation = rc_generation_count;
        metadata.flags = VP_STREAM_V2_THREAD_IDENTITY;
        if (vp_stream_writer_write_thread_v2(&rc_writer, &metadata) !=
            VP_RESULT_OK)
            return VP_ERROR_IO;
        ++rc_generation_count;
    }
    while ((uint64_t)sceKernelGetProcessTimeWide() < ready_deadline) {
        uint32_t ready = 0u;
        for (worker_index = 0u; worker_index < phase->worker_count;
             ++worker_index)
            ready += __atomic_load_n(
                &rc_workers[worker_index].ready, __ATOMIC_ACQUIRE);
        if (ready == phase->worker_count)
            return VP_RESULT_OK;
        sceKernelDelayThread(1000);
    }
    return VP_ERROR_PLATFORM;
}

static int rc_finish_phase_workers(const struct rc_phase_spec* phase)
{
    uint32_t worker_index;
    int result = VP_RESULT_OK;
    (void)phase;
    for (worker_index = 0u; worker_index < rc_active_worker_count;
         ++worker_index)
        __atomic_store_n(
            &rc_workers[worker_index].release, 1u, __ATOMIC_RELEASE);
    for (worker_index = 0u; worker_index < rc_active_worker_count;
         ++worker_index) {
        int status = -1;
        const SceUID thread = rc_workers[worker_index].thread_id;
        if (sceKernelWaitThreadEnd(thread, &status, NULL) < 0 ||
            sceKernelDeleteThread(thread) < 0)
            result = VP_ERROR_PLATFORM;
    }
    rc_active_worker_count = 0u;
    return result;
}

static void rc_abort_phase_workers(void)
{
    uint32_t worker_index;
    for (worker_index = 0u; worker_index < rc_active_worker_count;
         ++worker_index) {
        rc_workers[worker_index].duration_us = 0u;
        __atomic_store_n(
            &rc_workers[worker_index].start, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(
            &rc_workers[worker_index].release, 1u, __ATOMIC_RELEASE);
    }
    (void)rc_finish_phase_workers(NULL);
}

static int rc_run_phase(uint32_t phase_index)
{
    const struct rc_phase_spec* phase = &rc_phase_plan[phase_index];
    const uint32_t first_generation = rc_generation_count;
    uint64_t phase_start;
    uint64_t next_sample;
    uint32_t sample_round = 0u;
    uint32_t worker_index;
    int result;

    result = rc_start_phase_workers(phase, phase_index);
    if (result != VP_RESULT_OK) {
        rc_abort_phase_workers();
        return result;
    }
    rc_phases[phase_index].spec = phase;
    rc_phases[phase_index].first_event_index =
        (uint32_t)rc_writer.events_written;
    for (worker_index = 0u; worker_index < phase->worker_count;
         ++worker_index)
        __atomic_store_n(
            &rc_workers[worker_index].start, 1u, __ATOMIC_RELEASE);

    phase_start = (uint64_t)sceKernelGetProcessTimeWide();
    next_sample = phase_start;
    while ((uint64_t)sceKernelGetProcessTimeWide() - phase_start <
           phase->duration_us) {
        const uint64_t before_events = rc_writer.events_written;
        for (worker_index = 0u; worker_index < phase->worker_count;
             ++worker_index) {
            struct vp_vita_thread_snapshot snapshot;
            const uint32_t generation_index =
                first_generation + worker_index;
            result = vp_vita_record_thread(
                &rc_context, (uint32_t)rc_workers[worker_index].thread_id,
                &snapshot);
            if (result != VP_RESULT_OK)
                goto done;
            if (rc_generations[generation_index].first_event_index ==
                UINT32_MAX)
                rc_generations[generation_index].first_event_index =
                    (uint32_t)before_events + worker_index * 4u;
        }
        result = rc_drain();
        if (result != VP_RESULT_OK)
            goto done;
        for (worker_index = 0u; worker_index < phase->worker_count;
             ++worker_index) {
            const uint32_t generation_index =
                first_generation + worker_index;
            rc_generations[generation_index].last_event_index =
                (uint32_t)rc_writer.events_written - 1u;
        }
        ++sample_round;
        next_sample += RC_SAMPLE_INTERVAL_US;
        while ((uint64_t)sceKernelGetProcessTimeWide() < next_sample)
            sceKernelDelayThread(500);
    }
    rc_phases[phase_index].last_event_index =
        (uint32_t)rc_writer.events_written - 1u;
    result = sample_round >= 2u ? VP_RESULT_OK : VP_ERROR_STATE;

done:
    if (rc_finish_phase_workers(phase) != VP_RESULT_OK &&
        result == VP_RESULT_OK)
        result = VP_ERROR_PLATFORM;
    return result;
}

static int rc_write_metadata(
    const char* path, const struct vp_stats* ring,
    const struct vp_stream_writer_stats_v2* writer)
{
    SceUID fd;
    uint32_t index;
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
    if (fd < 0)
        return fd;
#define RC_WRITE_LITERAL(text) \
    do { if (rc_write_text(fd, (text)) < 0) goto write_failed; } while (0)
#define RC_WRITE_STRING_FIELD(name, value, comma) \
    do { \
        RC_WRITE_LITERAL("  \"" name "\": "); \
        if (rc_write_json_string(fd, (value)) < 0) goto write_failed; \
        RC_WRITE_LITERAL(comma "\n"); \
    } while (0)
    RC_WRITE_LITERAL("{\n");
    RC_WRITE_STRING_FIELD("format", "vitaprofiler-runclocks-experiment-v2",
                          ",");
    RC_WRITE_STRING_FIELD("experiment_id", VP_RUNCLOCKS_EXPERIMENT_ID, ",");
    RC_WRITE_STRING_FIELD("captured_at_utc", rc_captured_at_utc, ",");
    RC_WRITE_STRING_FIELD("device_model", VP_RUNCLOCKS_DEVICE_MODEL, ",");
    RC_WRITE_STRING_FIELD("device_id", VP_RUNCLOCKS_DEVICE_ID, ",");
    RC_WRITE_STRING_FIELD("firmware", VP_RUNCLOCKS_FIRMWARE, ",");
    RC_WRITE_STRING_FIELD("title_id", RC_TITLE_ID, ",");
    RC_WRITE_STRING_FIELD("build_id", VP_RUNCLOCKS_BUILD_ID, ",");
    RC_WRITE_STRING_FIELD(
        "workload",
        "fixed sleep and volatile integer phases; fresh workers per phase",
        ",");
    RC_WRITE_STRING_FIELD("clock_profile", VP_RUNCLOCKS_CLOCK_PROFILE, ",");
    RC_WRITE_STRING_FIELD("power_state", VP_RUNCLOCKS_POWER_STATE, ",");
    if (rc_write_format(
            fd,
            "  \"sample_interval_us\": %u,\n"
            "  \"sample_count_limit\": %u,\n"
            "  \"capture_duration_limit_us\": %u,\n"
            "  \"producer_dropped_events\": %u,\n"
            "  \"transport_lost_events\": %u,\n"
            "  \"sink_lost_events\": %u,\n",
            RC_SAMPLE_INTERVAL_US, RC_SAMPLE_LIMIT,
            RC_CAPTURE_LIMIT_US, ring->dropped,
            writer->transport_events_lost, writer->events_lost_to_sink) < 0)
        goto write_failed;
    RC_WRITE_LITERAL(
        "  \"reference_timer\": {\n"
        "    \"source\": \"sceKernelGetProcessTimeWide\",\n"
        "    \"unit\": \"microseconds\",\n"
        "    \"frequency_hz\": 1000000,\n"
        "    \"monotonic\": true\n"
        "  },\n"
        "  \"threads\": [\n");
    for (index = 0u; index < rc_generation_count; ++index) {
        const struct rc_generation_record* generation =
            &rc_generations[index];
        if (rc_write_format(
                fd,
                "    {\"thread_id\": \"0x%08x\", \"generation\": %u, "
                "\"label\": ",
                generation->thread_id, generation->generation) < 0 ||
            rc_write_json_string(fd, generation->label) < 0 ||
            rc_write_format(
                fd,
                ", \"first_event_index\": %u, \"last_event_index\": %u}%s\n",
                generation->first_event_index, generation->last_event_index,
                index + 1u == rc_generation_count ? "" : ",") < 0)
            goto write_failed;
    }
    RC_WRITE_LITERAL("  ],\n  \"phases\": [\n");
    for (index = 0u; index < RC_MAX_PHASES; ++index) {
        const struct rc_phase_record* phase = &rc_phases[index];
        RC_WRITE_LITERAL("    {\"phase_id\": ");
        if (rc_write_json_string(fd, phase->spec->phase_id) < 0 ||
            rc_write_text(fd, ", \"condition\": ") < 0 ||
            rc_write_json_string(fd, phase->spec->condition) < 0 ||
            rc_write_format(
                fd,
                ", \"repeat\": %u, \"target_duration_us\": %u, "
                "\"worker_count\": %u, \"first_event_index\": %u, "
                "\"last_event_index\": %u}%s\n",
                phase->spec->repeat, phase->spec->duration_us,
                phase->spec->worker_count, phase->first_event_index,
                phase->last_event_index,
                index + 1u == RC_MAX_PHASES ? "" : ",") < 0)
            goto write_failed;
    }
    RC_WRITE_LITERAL(
        "  ],\n"
        "  \"counter_bits\": null,\n"
        "  \"max_wrap_delta_raw\": null,\n"
        "  \"notes\": \"host-only preparation; hardware run requires "
        "coordinator authorization\"\n"
        "}\n");
    {
        const int sync_result = sceIoSyncByFd(fd, 0);
        const int close_result = sceIoClose(fd);
        if (sync_result < 0 || close_result < 0)
            return -1;
    }
    return sceIoSync("ux0:", 0);

write_failed:
    sceIoClose(fd);
    return -1;
#undef RC_WRITE_STRING_FIELD
#undef RC_WRITE_LITERAL
}

static int rc_run_experiment(void)
{
    char capture_path[128];
    char metadata_path[128];
    struct vp_name_dictionary_config name_config;
    struct vp_stream_writer_config writer_config;
    struct vp_stream_v2_session session;
    struct vp_stats ring_stats;
    struct vp_stream_writer_stats_v2 writer_stats;
    SceDateTime captured_at;
    uint64_t stream_start;
    uint32_t phase_index;
    int result;

    if (!rc_metadata_is_configured())
        return VP_ERROR_INVALID_ARGUMENT;
    memset(&captured_at, 0, sizeof(captured_at));
    if (sceRtcGetCurrentClockUtc(&captured_at) < 0 ||
        sceRtcCheckValid(&captured_at) < 0 ||
        snprintf(
            rc_captured_at_utc, sizeof(rc_captured_at_utc),
            "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ",
            (unsigned int)captured_at.year,
            (unsigned int)captured_at.month,
            (unsigned int)captured_at.day,
            (unsigned int)captured_at.hour,
            (unsigned int)captured_at.minute,
            (unsigned int)captured_at.second,
            (unsigned int)captured_at.microsecond) != 27)
        return VP_ERROR_PLATFORM;
    if (snprintf(capture_path, sizeof(capture_path), "%s/runclocks-%s.vptrace",
                 RC_OUTPUT_DIRECTORY, VP_RUNCLOCKS_EXPERIMENT_ID) < 0 ||
        snprintf(metadata_path, sizeof(metadata_path),
                 "%s/runclocks-%s.json", RC_OUTPUT_DIRECTORY,
                 VP_RUNCLOCKS_EXPERIMENT_ID) < 0)
        return VP_ERROR_CAPACITY;
    (void)sceIoMkdir(RC_OUTPUT_DIRECTORY, 0777);
    {
        SceIoStat stat;
        if (sceIoGetstat(capture_path, &stat) >= 0 ||
            sceIoGetstat(metadata_path, &stat) >= 0)
            return VP_ERROR_STATE;
    }
    rc_capture_fd = sceIoOpen(
        capture_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
    if (rc_capture_fd < 0)
        return VP_ERROR_IO;

    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = rc_name_entries;
    name_config.entry_capacity = 1u;
    name_config.text = rc_name_text;
    name_config.text_capacity = sizeof(rc_name_text);
    if (vp_name_dictionary_init(&rc_names, &name_config) != VP_RESULT_OK ||
        vp_name_dictionary_seal(&rc_names) != VP_RESULT_OK ||
        vp_vita_init(&rc_context, rc_slots, RC_RING_CAPACITY) !=
            VP_RESULT_OK) {
        result = VP_ERROR_STATE;
        goto close_capture;
    }
    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &rc_context;
    writer_config.names = &rc_names;
    writer_config.write = rc_file_sink;
    writer_config.write_user = &rc_capture_fd;
    writer_config.dictionary_buffer = rc_dictionary_buffer;
    writer_config.dictionary_buffer_capacity = sizeof(rc_dictionary_buffer);
    result = vp_stream_writer_init_v2(&rc_writer, &writer_config);
    if (result != VP_RESULT_OK)
        goto close_capture;
    stream_start = (uint64_t)sceKernelGetProcessTimeWide();
    memset(&session, 0, sizeof(session));
    session.session_id = stream_start | UINT64_C(1);
    session.timer_source = "sceKernelGetProcessTimeWide";
    session.timer_unit = "microseconds";
    session.flags = VP_STREAM_V2_SESSION_TIMER_SOURCE |
                    VP_STREAM_V2_SESSION_TIMER_UNIT;
    result = vp_stream_writer_begin_v2(&rc_writer, stream_start, &session);
    if (result != VP_RESULT_OK)
        goto close_capture;
    for (phase_index = 0u; phase_index < RC_MAX_PHASES; ++phase_index) {
        psvDebugScreenPrintf("Phase %u/%u: %s\n", phase_index + 1u,
                             RC_MAX_PHASES,
                             rc_phase_plan[phase_index].phase_id);
        result = rc_run_phase(phase_index);
        if (result != VP_RESULT_OK)
            goto close_capture;
    }
    result = rc_drain();
    if (result == VP_RESULT_OK)
        result = vp_stream_writer_close_v2(&rc_writer);
    if (result != VP_RESULT_OK ||
        vp_get_stats(&rc_context, &ring_stats) != VP_RESULT_OK ||
        vp_stream_writer_get_stats_v2(&rc_writer, &writer_stats) !=
            VP_RESULT_OK ||
        ring_stats.dropped != 0u ||
        writer_stats.transport_events_lost != 0u ||
        writer_stats.events_lost_to_sink != 0u) {
        result = VP_ERROR_STATE;
        goto close_capture;
    }
    {
        const int sync_result = sceIoSyncByFd(rc_capture_fd, 0);
        const int close_result = sceIoClose(rc_capture_fd);
        rc_capture_fd = -1;
        if (sync_result < 0 || close_result < 0)
            return VP_ERROR_IO;
    }
    if (sceIoSync("ux0:", 0) < 0)
        return VP_ERROR_IO;
    result = rc_write_metadata(metadata_path, &ring_stats, &writer_stats);
    if (result < 0)
        return VP_ERROR_IO;
    psvDebugScreenPrintf("\nCapture: %s\nMetadata: %s\n",
                         capture_path, metadata_path);
    return VP_RESULT_OK;

close_capture:
    if (rc_capture_fd >= 0) {
        sceIoClose(rc_capture_fd);
        rc_capture_fd = -1;
    }
    return result;
}

#endif

int main(void)
{
    psvDebugScreenInit();
    psvDebugScreenPrintf("VitaProfiler runClocks characterization\n");
    psvDebugScreenPrintf("Title: %s\n", RC_TITLE_ID);
    psvDebugScreenPrintf("Network use: none\n\n");
#if !VP_RUNCLOCKS_EXPERIMENT_ENABLED
    psvDebugScreenPrintf(
        "DISABLED at build time. No evidence was captured.\n"
        "Build only with the documented explicit enable and metadata.\n");
    sceKernelDelayThread(5000000);
    return 0;
#else
    psvDebugScreenPrintf(
        "Experiment: %s\n"
        "Bound: 5 seconds, 2 workers, 64 ring slots.\n"
        "Cross starts; Circle or 30-second timeout aborts.\n",
        VP_RUNCLOCKS_EXPERIMENT_ID);
    if (!rc_wait_for_confirmation()) {
        psvDebugScreenPrintf("\nAborted. No evidence was captured.\n");
        sceKernelDelayThread(3000000);
        return 0;
    }
    {
        const int result = rc_run_experiment();
        psvDebugScreenPrintf("\nResult: %s (0x%08x)\n",
                             result == VP_RESULT_OK ? "PASS" : "FAIL",
                             (unsigned int)result);
        psvDebugScreenPrintf(
            result == VP_RESULT_OK ?
                "Preserve both files and analyze them on the host.\n" :
                "Do not use partial output as evidence.\n");
        sceKernelDelayThread(10000000);
        return result == VP_RESULT_OK ? 0 : 1;
    }
#endif
}
