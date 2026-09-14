#include "uvdb_monitor.h"

#include <limits.h>
#include <string.h>

#define STRING_SIZE(value) (sizeof(value) - 1u)

static const char truncated_marker[] = "... output truncated\n";

void uvdb_monitor_display_stop_reset(
    struct uvdb_monitor_display_stop* stop)
{
    if(stop)
        memset(stop, 0, sizeof(*stop));
}

int uvdb_monitor_display_stop_begin(
    struct uvdb_monitor_display_stop* stop,
    uint32_t generation)
{
    if(!stop || !generation ||
       stop->phase != UVDB_MONITOR_DISPLAY_STOP_IDLE)
        return -1;
    stop->generation = generation;
    stop->phase = UVDB_MONITOR_DISPLAY_STOP_NEEDS_SAMPLE;
    return 0;
}

uint32_t uvdb_monitor_display_stop_pending_generation(
    const struct uvdb_monitor_display_stop* stop)
{
    if(!stop || stop->phase != UVDB_MONITOR_DISPLAY_STOP_NEEDS_SAMPLE)
        return 0;
    return stop->generation;
}

int uvdb_monitor_display_stop_arm(
    struct uvdb_monitor_display_stop* stop,
    uint32_t generation)
{
    if(!stop || !generation ||
       stop->phase != UVDB_MONITOR_DISPLAY_STOP_NEEDS_SAMPLE ||
       stop->generation != generation)
        return -1;
    stop->phase = UVDB_MONITOR_DISPLAY_STOP_ARMED;
    return 0;
}

enum uvdb_monitor_display_stop_action uvdb_monitor_display_stop_on_exception(
    struct uvdb_monitor_display_stop* stop,
    int is_initial_stop_trap)
{
    if(!stop)
        return UVDB_MONITOR_DISPLAY_STOP_NOT_OURS;

    if(is_initial_stop_trap)
    {
        if(stop->phase == UVDB_MONITOR_DISPLAY_STOP_ARMED)
        {
            uvdb_monitor_display_stop_reset(stop);
            return UVDB_MONITOR_DISPLAY_STOP_HANDLE;
        }
        if(stop->phase == UVDB_MONITOR_DISPLAY_STOP_CANCELLED)
        {
            uvdb_monitor_display_stop_reset(stop);
            return UVDB_MONITOR_DISPLAY_STOP_IGNORE;
        }
        return UVDB_MONITOR_DISPLAY_STOP_NOT_OURS;
    }

    if(stop->phase == UVDB_MONITOR_DISPLAY_STOP_NEEDS_SAMPLE)
        uvdb_monitor_display_stop_reset(stop);
    else if(stop->phase == UVDB_MONITOR_DISPLAY_STOP_ARMED)
        stop->phase = UVDB_MONITOR_DISPLAY_STOP_CANCELLED;
    return UVDB_MONITOR_DISPLAY_STOP_NOT_OURS;
}

int uvdb_monitor_display_generation_is_current(
    uint32_t sample_generation,
    int active_socket,
    uint32_t active_generation)
{
    return active_socket >= 0 && sample_generation &&
           sample_generation == active_generation;
}

struct monitor_builder {
    char* output;
    size_t capacity;
    size_t copied;
    size_t required;
    int overflow;
};

static int hex_value(char digit)
{
    if(digit >= '0' && digit <= '9')
        return digit - '0';
    if(digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if(digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

int uvdb_monitor_ieee754_to_millihz(
    uint32_t bits,
    uint32_t* millihz)
{
    if(!millihz || (bits >> 31u))
        return -1;
    uint32_t exponent = (bits >> 23u) & 0xffu;
    uint32_t fraction = bits & 0x7fffffu;
    if(exponent == 0xffu)
        return -1;

    uint64_t significand;
    int shift;
    if(exponent)
    {
        significand = 0x800000u | fraction;
        shift = (int)exponent - 150;
    }
    else
    {
        significand = fraction;
        shift = -149;
    }

    uint64_t scaled = significand * 1000u;
    if(shift >= 0)
    {
        if(shift >= 64 ||
           scaled > ((uint64_t)UINT32_MAX >> (unsigned int)shift))
            return -1;
        scaled <<= shift;
    }
    else
    {
        unsigned int right = (unsigned int)-shift;
        if(right >= 64u)
            scaled = 0;
        else if(right)
            scaled = (scaled + (UINT64_C(1) << (right - 1u))) >> right;
    }
    if(scaled > 1000000u)
        return -1;
    *millihz = (uint32_t)scaled;
    return 0;
}

static int command_equals(const char* command, size_t size, const char* value)
{
    size_t value_size = strlen(value);
    return size == value_size && !memcmp(command, value, size);
}

int uvdb_monitor_parse_qrcmd(
    const char* packet,
    size_t packet_size,
    enum uvdb_monitor_command* command)
{
    static const char prefix[] = "qRcmd,";
    if(!packet || !command || packet_size < STRING_SIZE(prefix) ||
       memcmp(packet, prefix, STRING_SIZE(prefix)))
        return UVDB_MONITOR_PARSE_MALFORMED;

    *command = UVDB_MONITOR_COMMAND_NONE;
    size_t encoded_size = packet_size - STRING_SIZE(prefix);
    if(encoded_size & 1u)
        return UVDB_MONITOR_PARSE_MALFORMED;
    size_t decoded_size = encoded_size / 2u;
    if(decoded_size > UVDB_MONITOR_COMMAND_TEXT_MAX)
        return UVDB_MONITOR_PARSE_MALFORMED;

    char decoded[UVDB_MONITOR_COMMAND_TEXT_MAX];
    for(size_t i = 0; i < decoded_size; ++i)
    {
        int high = hex_value(packet[STRING_SIZE(prefix) + i * 2u]);
        int low = hex_value(packet[STRING_SIZE(prefix) + i * 2u + 1u]);
        if(high < 0 || low < 0)
            return UVDB_MONITOR_PARSE_MALFORMED;
        unsigned int value = (unsigned int)((high << 4) | low);
        if((value < 0x20u && value != '\t') || value > 0x7eu)
            return UVDB_MONITOR_PARSE_MALFORMED;
        decoded[i] = (char)value;
    }

    size_t start = 0;
    while(start < decoded_size &&
          (decoded[start] == ' ' || decoded[start] == '\t'))
        start++;
    size_t end = decoded_size;
    while(end > start &&
          (decoded[end - 1u] == ' ' || decoded[end - 1u] == '\t'))
        end--;

    const char* text = decoded + start;
    size_t size = end - start;
    if(command_equals(text, size, "help"))
        *command = UVDB_MONITOR_COMMAND_HELP;
    else if(command_equals(text, size, "status"))
        *command = UVDB_MONITOR_COMMAND_STATUS;
    else if(command_equals(text, size, "threads"))
        *command = UVDB_MONITOR_COMMAND_THREADS;
    else if(command_equals(text, size, "modules"))
        *command = UVDB_MONITOR_COMMAND_MODULES;
    else if(command_equals(text, size, "console"))
        *command = UVDB_MONITOR_COMMAND_CONSOLE;
    else if(command_equals(text, size, "display"))
        *command = UVDB_MONITOR_COMMAND_DISPLAY;
    else
        return UVDB_MONITOR_PARSE_UNKNOWN;
    return UVDB_MONITOR_PARSE_OK;
}

static void append_bytes(
    struct monitor_builder* builder,
    const char* text,
    size_t size)
{
    if(builder->overflow)
        return;
    if(size > SIZE_MAX - builder->required)
    {
        builder->overflow = 1;
        return;
    }
    size_t available = builder->capacity - builder->copied;
    size_t copy_size = size < available ? size : available;
    if(copy_size)
        memcpy(builder->output + builder->copied, text, copy_size);
    builder->copied += copy_size;
    builder->required += size;
}

static void append_string(struct monitor_builder* builder, const char* text)
{
    append_bytes(builder, text, strlen(text));
}

static void append_char(struct monitor_builder* builder, char value)
{
    append_bytes(builder, &value, 1u);
}

static void append_u64_decimal(struct monitor_builder* builder, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while(value && count < sizeof(digits));
    while(count)
        append_char(builder, digits[--count]);
}

static void append_hex32(struct monitor_builder* builder, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    append_string(builder, "0x");
    for(int shift = 28; shift >= 0; shift -= 4)
        append_char(builder, digits[(value >> shift) & 0xfu]);
}

static void append_thread_selector(
    struct monitor_builder* builder,
    int32_t thread)
{
    if(thread == 0)
        append_string(builder, "any");
    else if(thread == -1)
        append_string(builder, "all");
    else if(thread > 0)
        append_hex32(builder, (uint32_t)thread);
    else
        append_string(builder, "invalid");
}

static void append_sanitized_name(
    struct monitor_builder* builder,
    const char* name,
    size_t capacity)
{
    size_t count = 0;
    while(count < capacity && name[count])
    {
        unsigned char value = (unsigned char)name[count++];
        append_char(builder,
                    value >= 0x20u && value <= 0x7eu ? (char)value : '_');
    }
    if(!count)
        append_string(builder, "unnamed");
}

static const char* state_name(enum uvdb_monitor_state state)
{
    switch(state)
    {
        case UVDB_MONITOR_STATE_IDLE: return "idle";
        case UVDB_MONITOR_STATE_LISTENING: return "listening";
        case UVDB_MONITOR_STATE_CONNECTED: return "connected";
        case UVDB_MONITOR_STATE_ERROR: return "error";
    }
    return "invalid";
}

static const char* fault_name(int32_t fault)
{
    switch(fault)
    {
        case 0: return "data-abort";
        case 1: return "prefetch-abort";
        case 2: return "undefined-instruction";
    }
    return "unknown";
}

static void render_help(struct monitor_builder* builder)
{
    append_string(builder,
        "VitaDebugger read-only monitor commands\n"
        "  help     Show this command list\n"
        "  status   Show debugger, stop-session, and fault state\n"
        "  threads  List stopped process threads and selections\n"
        "  modules  List loaded modules and memory segments\n"
        "  console  Show bounded GDB console queue and transport statistics\n"
        "  display  Show read-only framebuffer and display state\n");
}

static void render_status(
    struct monitor_builder* builder,
    const struct uvdb_monitor_status* status)
{
    append_string(builder, "VitaDebugger status\n  state: ");
    append_string(builder, state_name(status->state));
    append_string(builder, "\n  target: ");
    append_string(builder, status->target_stopped ? "stopped" : "running");
    append_string(builder, ", signal=");
    append_u64_decimal(builder, status->stop_signal < 0 ? 0u :
                       (uint32_t)status->stop_signal);
    append_string(builder, "\n  transport: ");
    append_string(builder, status->no_ack_mode ? "no-ack" : "ack");
    append_string(builder, ", port=");
    append_u64_decimal(builder, status->port);
    append_string(builder, ", PacketSize=");
    append_hex32(builder, status->packet_size);
    append_string(builder, "\n  threads: ");
    append_u64_decimal(builder, status->thread_count);
    append_string(builder, "\n  selection: stopped=");
    append_thread_selector(builder, status->stopped_thread);
    append_string(builder, " Hg=");
    append_thread_selector(builder, status->general_thread);
    append_string(builder, " Hc=");
    append_thread_selector(builder, status->resume_thread);
    append_string(builder, " exception=");
    append_thread_selector(builder, status->exception_thread);
    append_string(builder, "\n  software-breakpoints: ");
    append_u64_decimal(builder, status->software_breakpoint_count);

    append_string(builder, "\n  kernel: ");
    if(!status->kernel_compiled)
        append_string(builder, "library-only");
    else if(!status->kernel_status_available)
        append_string(builder, "unavailable");
    else
    {
        append_string(builder, status->kernel_compatible ? "compatible" :
                                                          "incompatible");
        append_string(builder, ", ABI=");
        append_hex32(builder, status->kernel_abi);
        append_string(builder, ", capabilities=");
        append_hex32(builder, status->kernel_capabilities);
        append_string(builder, ", max-threads=");
        append_u64_decimal(builder, status->kernel_max_threads);
    }

    append_string(builder, "\n  stop-session: ");
    if(!status->kernel_compiled)
        append_string(builder, "unavailable");
    else if(!status->stop_session_active)
        append_string(builder, status->stop_session_failed ? "failed" :
                                                          "inactive");
    else
        append_string(builder, status->stop_session_failed ? "failed" :
                                                          "active, healthy");
    append_string(builder, "\n  VFP reads: ");
    append_string(builder, status->vfp_reads_enabled ? "enabled" : "disabled");

    append_string(builder, "\n  last-fault: ");
    if(!status->last_fault_available)
        append_string(builder, "none");
    else
    {
        append_string(builder, fault_name(status->last_fault_type));
        append_string(builder, ", status=");
        append_hex32(builder, status->last_fault_status);
        append_string(builder, ", address=");
        append_hex32(builder, status->last_fault_address);
        append_string(builder, ", pc=");
        append_hex32(builder, status->last_fault_pc);
    }
    append_char(builder, '\n');
}

static void append_thread_flags(
    struct monitor_builder* builder,
    uint32_t flags)
{
    int wrote = 0;
    append_char(builder, '[');
#define APPEND_FLAG(mask, text) \
    do { \
        if(flags & (mask)) { \
            if(wrote) append_char(builder, ','); \
            append_string(builder, (text)); \
            wrote = 1; \
        } \
    } while(0)
    APPEND_FLAG(UVDB_MONITOR_THREAD_STOPPED, "stopped");
    APPEND_FLAG(UVDB_MONITOR_THREAD_GENERAL, "Hg");
    APPEND_FLAG(UVDB_MONITOR_THREAD_RESUME, "Hc");
    APPEND_FLAG(UVDB_MONITOR_THREAD_EXCEPTION, "exception");
#undef APPEND_FLAG
    if(!wrote)
        append_char(builder, '-');
    append_char(builder, ']');
}

static void render_threads(
    struct monitor_builder* builder,
    const struct uvdb_monitor_snapshot* snapshot)
{
    append_string(builder, "VitaDebugger threads (");
    append_u64_decimal(builder, snapshot->thread_count);
    append_string(builder, ")\n");
    for(size_t i = 0; i < snapshot->thread_count; ++i)
    {
        const struct uvdb_monitor_thread* thread = &snapshot->threads[i];
        append_string(builder, "  ");
        append_hex32(builder, (uint32_t)thread->id);
        append_char(builder, ' ');
        append_thread_flags(builder, thread->flags);
        append_char(builder, ' ');
        append_sanitized_name(builder, thread->name,
                              sizeof(thread->name));
        append_char(builder, '\n');
    }
}

static void render_modules(
    struct monitor_builder* builder,
    const struct uvdb_monitor_snapshot* snapshot)
{
    append_string(builder, "VitaDebugger modules\n");
    if(snapshot->module_query_result < 0)
    {
        append_string(builder, "  unavailable: ");
        append_hex32(builder, (uint32_t)snapshot->module_query_result);
        append_char(builder, '\n');
        return;
    }

    append_string(builder, "  shown=");
    append_u64_decimal(builder, snapshot->module_count);
    append_string(builder, " reported=");
    append_u64_decimal(builder, snapshot->module_reported_count);
    append_string(builder, " skipped=");
    append_u64_decimal(builder, snapshot->module_skipped_count);
    if(snapshot->module_reported_count > snapshot->module_scanned_count)
    {
        append_string(builder, " snapshot-omitted=");
        append_u64_decimal(builder, snapshot->module_reported_count -
                           snapshot->module_scanned_count);
    }
    append_char(builder, '\n');

    for(size_t i = 0; i < snapshot->module_count; ++i)
    {
        const struct uvdb_monitor_module* module = &snapshot->modules[i];
        append_string(builder, "  ");
        append_hex32(builder, module->id);
        append_char(builder, ' ');
        append_sanitized_name(builder, module->name,
                              sizeof(module->name));
        append_char(builder, '\n');
        for(size_t segment = 0; segment < module->segment_count; ++segment)
        {
            const struct uvdb_monitor_segment* entry =
                &module->segments[segment];
            append_string(builder, "    segment[");
            append_u64_decimal(builder, entry->index);
            append_string(builder, "]: address=");
            append_hex32(builder, entry->address);
            append_string(builder, " size=");
            append_hex32(builder, entry->memory_size);
            append_string(builder, " perms=");
            append_hex32(builder, entry->permissions);
            append_char(builder, '\n');
        }
    }
}

static void append_record_bytes(
    struct monitor_builder* builder,
    uint32_t records,
    uint32_t bytes)
{
    append_u64_decimal(builder, records);
    append_string(builder, " records / ");
    append_u64_decimal(builder, bytes);
    append_string(builder, " bytes");
}

static void render_console(
    struct monitor_builder* builder,
    const struct uvdb_monitor_console* console)
{
    append_string(builder, "VitaDebugger console\n");
    if(!console->available)
    {
        append_string(builder, "  unavailable\n");
        return;
    }

    append_string(builder, "  session: ");
    append_string(builder, console->session_open ? "open" : "closed");
    append_string(builder, ", generation=");
    append_u64_decimal(builder, console->session_generation);
    append_string(builder, ", no-ack=");
    append_string(builder, console->no_ack_mode ? "yes" : "no");
    append_string(builder, ", transport-failed=");
    append_string(builder, console->transport_failed ? "yes" : "no");

    append_string(builder, "\n  queued: ");
    append_record_bytes(builder, console->queued_records,
                        console->queued_bytes);
    append_string(builder, "\n  accepted: ");
    append_record_bytes(builder, console->accepted_records,
                        console->accepted_bytes);
    append_string(builder, "\n  sent: ");
    append_record_bytes(builder, console->sent_records,
                        console->sent_bytes);

    append_string(builder, "\n  dropped-disconnected: ");
    append_record_bytes(builder, console->dropped_disconnected_records,
                        console->dropped_disconnected_bytes);
    append_string(builder, "\n  dropped-contention: ");
    append_record_bytes(builder, console->dropped_contention_records,
                        console->dropped_contention_bytes);
    append_string(builder, "\n  dropped-full: ");
    append_record_bytes(builder, console->dropped_full_records,
                        console->dropped_full_bytes);
    append_string(builder, "\n  dropped-stale: ");
    append_record_bytes(builder, console->dropped_stale_records,
                        console->dropped_stale_bytes);

    uint64_t dropped_records =
        (uint64_t)console->dropped_disconnected_records +
        console->dropped_contention_records +
        console->dropped_full_records + console->dropped_stale_records;
    uint64_t dropped_bytes =
        (uint64_t)console->dropped_disconnected_bytes +
        console->dropped_contention_bytes +
        console->dropped_full_bytes + console->dropped_stale_bytes;
    append_string(builder, "\n  dropped-total: ");
    append_u64_decimal(builder, dropped_records);
    append_string(builder, " records / ");
    append_u64_decimal(builder, dropped_bytes);
    append_string(builder, " bytes");

    append_string(builder, "\n  lifecycle: sessions=");
    append_u64_decimal(builder, console->sessions_opened);
    append_string(builder, " reconnects=");
    append_u64_decimal(builder, console->reconnects);
    append_string(builder, "\n  transport: frames=");
    append_u64_decimal(builder, console->frames_sent);
    append_string(builder, " frame-bytes=");
    append_u64_decimal(builder, console->frame_bytes_sent);
    append_string(builder, " would-block=");
    append_u64_decimal(builder, console->would_block);
    append_string(builder, " commit-busy=");
    append_u64_decimal(builder, console->commit_busy);
    append_string(builder, "\n  transport-errors: partial=");
    append_u64_decimal(builder, console->partial_writes);
    append_string(builder, " hard=");
    append_u64_decimal(builder, console->hard_errors);
    append_string(builder, " session=");
    append_u64_decimal(builder, console->session_errors);
    append_string(builder, " last-native=");
    append_hex32(builder, (uint32_t)console->last_native_error);
    append_char(builder, '\n');
}

static const char* pixel_format_name(uint32_t format)
{
    if(format == 0x00000000u)
        return "A8B8G8R8";
    if(format == 0x60800000u)
        return "A2B10G10R10";
    return "unknown";
}

static void render_framebuffer(
    struct monitor_builder* builder,
    const char* label,
    const struct uvdb_monitor_framebuffer* framebuffer)
{
    append_string(builder, "  framebuffer-");
    append_string(builder, label);
    append_string(builder, ": ");
    if(framebuffer->query_result < 0)
    {
        append_string(builder, "unavailable, result=");
        append_hex32(builder, (uint32_t)framebuffer->query_result);
        append_char(builder, '\n');
        return;
    }
    append_string(builder, "address=");
    append_hex32(builder, framebuffer->address);
    append_string(builder, " size=");
    append_u64_decimal(builder, framebuffer->width);
    append_char(builder, 'x');
    append_u64_decimal(builder, framebuffer->height);
    append_string(builder, " pitch=");
    append_u64_decimal(builder, framebuffer->pitch);
    append_string(builder, " format=");
    append_string(builder, pixel_format_name(framebuffer->pixel_format));
    append_char(builder, '(');
    append_hex32(builder, framebuffer->pixel_format);
    append_string(builder, ")\n");
}

static void render_display(
    struct monitor_builder* builder,
    const struct uvdb_monitor_display* display)
{
    append_string(builder, "VitaDebugger display\n");
    if(!display->available)
    {
        append_string(builder,
                      "  unavailable: no safe cached server-thread sample\n");
        return;
    }

    append_string(builder, "  primary-head: ");
    if(display->primary_head < 0)
    {
        append_string(builder, "unavailable, result=");
        append_hex32(builder, (uint32_t)display->primary_head);
    }
    else
        append_u64_decimal(builder, (uint32_t)display->primary_head);
    append_string(builder, "\n  vcount: ");
    if(display->vcount < 0)
    {
        append_string(builder, "unavailable, result=");
        append_hex32(builder, (uint32_t)display->vcount);
    }
    else
        append_u64_decimal(builder, (uint32_t)display->vcount);

    append_string(builder, "\n  refresh-rate: ");
    if(display->refresh_query_result < 0)
    {
        append_string(builder, "unavailable, result=");
        append_hex32(builder, (uint32_t)display->refresh_query_result);
    }
    else
    {
        append_u64_decimal(builder, display->refresh_millihz / 1000u);
        append_char(builder, '.');
        uint32_t fraction = display->refresh_millihz % 1000u;
        append_char(builder, (char)('0' + fraction / 100u));
        append_char(builder, (char)('0' + (fraction / 10u) % 10u));
        append_char(builder, (char)('0' + fraction % 10u));
        append_string(builder, " Hz");
    }

    append_string(builder, "\n  maximum-framebuffer: ");
    if(display->maximum_query_result < 0)
    {
        append_string(builder, "unavailable, result=");
        append_hex32(builder, (uint32_t)display->maximum_query_result);
    }
    else
    {
        append_u64_decimal(builder, display->maximum_width);
        append_char(builder, 'x');
        append_u64_decimal(builder, display->maximum_height);
    }
    append_char(builder, '\n');
    render_framebuffer(builder, "immediate", &display->immediate);
    render_framebuffer(builder, "next", &display->next_frame);
}

static int snapshot_valid(
    enum uvdb_monitor_command command,
    const struct uvdb_monitor_snapshot* snapshot)
{
    if(command == UVDB_MONITOR_COMMAND_HELP)
        return 1;
    if(!snapshot || snapshot->status.state > UVDB_MONITOR_STATE_ERROR ||
       snapshot->thread_count > UVDB_MONITOR_MAX_THREADS ||
       snapshot->module_count > UVDB_MONITOR_MAX_MODULES)
        return 0;
    if(command == UVDB_MONITOR_COMMAND_THREADS &&
       snapshot->thread_count && !snapshot->threads)
        return 0;
    if(command == UVDB_MONITOR_COMMAND_MODULES)
    {
        if(snapshot->module_count && !snapshot->modules)
            return 0;
        for(size_t i = 0; i < snapshot->module_count; ++i)
            if(snapshot->modules[i].segment_count >
               UVDB_MONITOR_MAX_SEGMENTS)
                return 0;
    }
    return command == UVDB_MONITOR_COMMAND_STATUS ||
           command == UVDB_MONITOR_COMMAND_THREADS ||
           command == UVDB_MONITOR_COMMAND_MODULES ||
           command == UVDB_MONITOR_COMMAND_CONSOLE ||
           command == UVDB_MONITOR_COMMAND_DISPLAY;
}

int uvdb_monitor_render(
    enum uvdb_monitor_command command,
    const struct uvdb_monitor_snapshot* snapshot,
    char* output,
    size_t capacity,
    size_t* output_size)
{
    if(!output_size)
        return UVDB_MONITOR_RENDER_INVALID;
    *output_size = 0;
    if(!output || capacity < STRING_SIZE(truncated_marker) ||
       !snapshot_valid(command, snapshot))
        return UVDB_MONITOR_RENDER_INVALID;

    struct monitor_builder builder = {
        .output = output,
        .capacity = capacity,
    };
    switch(command)
    {
        case UVDB_MONITOR_COMMAND_HELP:
            render_help(&builder);
            break;
        case UVDB_MONITOR_COMMAND_STATUS:
            render_status(&builder, &snapshot->status);
            break;
        case UVDB_MONITOR_COMMAND_THREADS:
            render_threads(&builder, snapshot);
            break;
        case UVDB_MONITOR_COMMAND_MODULES:
            render_modules(&builder, snapshot);
            break;
        case UVDB_MONITOR_COMMAND_CONSOLE:
            render_console(&builder, &snapshot->console);
            break;
        case UVDB_MONITOR_COMMAND_DISPLAY:
            render_display(&builder, &snapshot->display);
            break;
        default:
            return UVDB_MONITOR_RENDER_INVALID;
    }

    if(builder.overflow)
        return UVDB_MONITOR_RENDER_INVALID;
    if(builder.required <= capacity)
    {
        *output_size = builder.required;
        return UVDB_MONITOR_RENDER_OK;
    }

    size_t prefix = capacity - STRING_SIZE(truncated_marker);
    size_t line_end = prefix;
    while(line_end && output[line_end - 1u] != '\n')
        line_end--;
    if(line_end)
        prefix = line_end;
    else
        prefix = 0;
    memcpy(output + prefix, truncated_marker,
           STRING_SIZE(truncated_marker));
    *output_size = prefix + STRING_SIZE(truncated_marker);
    return UVDB_MONITOR_RENDER_TRUNCATED;
}
