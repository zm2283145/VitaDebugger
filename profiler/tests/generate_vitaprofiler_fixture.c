#include "vitaprofiler_gpu.h"
#include "vitaprofiler_stream.h"

#include <stdio.h>
#include <string.h>

struct fixture_source {
    uint64_t now;
    uint32_t thread_id;
};

static uint64_t fixture_clock(void* user)
{
    return ((struct fixture_source*)user)->now;
}

static uint32_t fixture_thread(void* user)
{
    return ((struct fixture_source*)user)->thread_id;
}

static int file_sink(void* user, const uint8_t* data, size_t size)
{
    FILE* output = (FILE*)user;
    return fwrite(data, 1u, size, output) == size ? 0 : -1;
}

static int generate_fixture(const char* path)
{
    struct vp_context context;
    struct vp_slot slots[16];
    struct fixture_source source = {1000u, 7u};
    struct vp_config config;
    struct vp_name_dictionary names;
    struct vp_name_entry entries[32];
    char name_text[1024];
    struct vp_name_dictionary_config name_config;
    uint32_t update_id = 0u;
    uint32_t draws_id = 0u;
    uint32_t frame_id = 0u;
    struct vp_graphics_name_ids graphics_ids;
    struct vp_graphics_hooks graphics;
    struct vp_zone_scope graphics_draw;
    struct vp_event events[6];
    uint8_t dictionary_buffer[4096];
    struct vp_stream_writer writer;
    struct vp_stream_writer_config writer_config;
    size_t drained = 0u;
    FILE* output = NULL;
    int result = 1;

    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = 16u;
    config.clock = fixture_clock;
    config.clock_user = &source;
    config.thread_id = fixture_thread;
    config.thread_user = &source;
    if (vp_init(&context, &config) != VP_RESULT_OK)
        goto done;

    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = entries;
    name_config.entry_capacity = 32u;
    name_config.text = name_text;
    name_config.text_capacity = sizeof(name_text);
    if (vp_name_dictionary_init(&names, &name_config) != VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "update", &update_id) !=
            VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "draw calls", &draws_id) !=
            VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "main frame", &frame_id) !=
            VP_RESULT_OK ||
        vp_graphics_register_extended_names(&names, &graphics_ids) !=
            VP_RESULT_OK ||
        vp_name_dictionary_seal(&names) != VP_RESULT_OK ||
        vp_graphics_hooks_init(&graphics, &context, &graphics_ids) !=
            VP_RESULT_OK)
        goto done;

    memset(events, 0, sizeof(events));
    events[0] = (struct vp_event){1000u, 0, update_id, 7u, 1u,
                                  VP_EVENT_ZONE_BEGIN, 0u};
    events[1] = (struct vp_event){1100u, 42, draws_id, 7u, 0u,
                                  VP_EVENT_COUNTER, 0u};
    events[2] = (struct vp_event){1200u, 0, frame_id, 7u, 0u,
                                  VP_EVENT_FRAME, VP_EVENT_FLAG_FIRST};
    events[3] = (struct vp_event){17000u, 15800, frame_id, 7u, 1u,
                                  VP_EVENT_FRAME, 0u};
    events[4] = (struct vp_event){1250u, 250, update_id, 7u, 1u,
                                  VP_EVENT_ZONE_END, 0u};
    events[5] = (struct vp_event){1300u, 1024,
                                  VP_METRIC_FREE_USER_BYTES, 7u, 0u,
                                  VP_EVENT_MEMORY_SAMPLE, 0u};
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i)
        if (vp_record(&context, &events[i]) != VP_RESULT_OK)
            goto done;
    source.now = 18000u;
    if (vp_graphics_frame_mark(&graphics, VP_GRAPHICS_FRAME) !=
            VP_RESULT_OK ||
        vp_graphics_zone_begin(&graphics,
                               VP_GRAPHICS_ZONE_VITAGL_DRAW_SUBMIT,
                               &graphics_draw) != VP_RESULT_OK)
        goto done;
    source.now = 18025u;
    if (vp_graphics_zone_end(&graphics, &graphics_draw) != VP_RESULT_OK ||
        vp_graphics_counter(&graphics,
                            VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS,
                            43) != VP_RESULT_OK)
        goto done;

    output = fopen(path, "wb");
    if (output == NULL)
        goto done;
    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &context;
    writer_config.names = &names;
    writer_config.write = file_sink;
    writer_config.write_user = output;
    writer_config.dictionary_buffer = dictionary_buffer;
    writer_config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    if (vp_stream_writer_init(&writer, &writer_config) != VP_RESULT_OK ||
        vp_stream_writer_begin(&writer, 900u) != VP_RESULT_OK ||
        vp_stream_writer_drain(&writer, 16u, &drained) != VP_RESULT_OK ||
        drained != 10u ||
        vp_stream_writer_close(&writer) != VP_RESULT_OK ||
        fflush(output) != 0)
        goto done;
    if (fclose(output) != 0) {
        output = NULL;
        goto done;
    }
    output = NULL;
    result = 0;

done:
    if (output != NULL)
        fclose(output);
    if (result != 0)
        remove(path);
    return result;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fputs("usage: generate_vitaprofiler_fixture OUTPUT\n", stderr);
        return 2;
    }
    return generate_fixture(argv[1]);
}
