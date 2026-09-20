#pragma once

#include <stddef.h>
#include <stdint.h>

#define UVDB_MONITOR_COMMAND_TEXT_MAX 32u
#define UVDB_MONITOR_THREAD_NAME_MAX 32u
#define UVDB_MONITOR_MODULE_NAME_MAX 32u
#define UVDB_MONITOR_MAX_THREADS 64u
#define UVDB_MONITOR_MAX_MODULES 128u
#define UVDB_MONITOR_MAX_SEGMENTS 4u
#define UVDB_MONITOR_STOP_TRACE_COUNT 8u

enum uvdb_monitor_command {
    UVDB_MONITOR_COMMAND_NONE = 0,
    UVDB_MONITOR_COMMAND_HELP,
    UVDB_MONITOR_COMMAND_STATUS,
    UVDB_MONITOR_COMMAND_THREADS,
    UVDB_MONITOR_COMMAND_MODULES,
    UVDB_MONITOR_COMMAND_CONSOLE,
    UVDB_MONITOR_COMMAND_DISPLAY,
};

enum uvdb_monitor_parse_result {
    UVDB_MONITOR_PARSE_MALFORMED = -1,
    UVDB_MONITOR_PARSE_OK = 0,
    UVDB_MONITOR_PARSE_UNKNOWN = 1,
};

enum uvdb_monitor_render_result {
    UVDB_MONITOR_RENDER_INVALID = -1,
    UVDB_MONITOR_RENDER_OK = 0,
    UVDB_MONITOR_RENDER_TRUNCATED = 1,
};

enum uvdb_monitor_state {
    UVDB_MONITOR_STATE_IDLE = 0,
    UVDB_MONITOR_STATE_LISTENING,
    UVDB_MONITOR_STATE_CONNECTED,
    UVDB_MONITOR_STATE_ERROR,
};

enum uvdb_monitor_stop_injection_operation {
    UVDB_MONITOR_STOP_INJECTION_NONE = 0,
    UVDB_MONITOR_STOP_INJECTION_RENEW = 1,
    UVDB_MONITOR_STOP_INJECTION_END = 2,
};

enum uvdb_monitor_stop_injection_outcome {
    UVDB_MONITOR_STOP_INJECTION_OUTCOME_NONE = 0,
    UVDB_MONITOR_STOP_INJECTION_OUTCOME_INJECTED = 1,
    UVDB_MONITOR_STOP_INJECTION_OUTCOME_STALE = 2,
};

enum uvdb_monitor_stop_trace_wait_result {
    UVDB_MONITOR_STOP_TRACE_WAIT_NONE = 0,
    UVDB_MONITOR_STOP_TRACE_WAIT_CLEAR,
    UVDB_MONITOR_STOP_TRACE_WAIT_SAME_OWNER,
    UVDB_MONITOR_STOP_TRACE_WAIT_RELEASED,
    UVDB_MONITOR_STOP_TRACE_WAIT_TIMEOUT,
};

enum uvdb_monitor_stop_trace_predecessor_reason {
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_NONE = 0,
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_GUARD_CLOSED,
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_GUARD_NESTED,
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_SESSION_UNCLAIMABLE,
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_PROTOCOL_CONTENTION,
    UVDB_MONITOR_STOP_TRACE_PREDECESSOR_STATE_LOCK_CONTENTION,
};

/* The first display sample for a server-owned connection must be collected
 * after accept, but SceDisplay must never be called from the exception path.
 * This small state machine hands that sample to a deferred synthetic stop and
 * suppresses it if a real fault wins the race. */
enum uvdb_monitor_display_stop_phase {
    UVDB_MONITOR_DISPLAY_STOP_IDLE = 0,
    UVDB_MONITOR_DISPLAY_STOP_NEEDS_SAMPLE,
    UVDB_MONITOR_DISPLAY_STOP_ARMED,
    UVDB_MONITOR_DISPLAY_STOP_CANCELLED,
};

enum uvdb_monitor_display_stop_action {
    UVDB_MONITOR_DISPLAY_STOP_NOT_OURS = 0,
    UVDB_MONITOR_DISPLAY_STOP_HANDLE,
    UVDB_MONITOR_DISPLAY_STOP_IGNORE,
};

struct uvdb_monitor_display_stop {
    uint32_t generation;
    enum uvdb_monitor_display_stop_phase phase;
};

struct uvdb_monitor_stop_trace {
    uint32_t generation;
    int32_t active;
    int32_t thread;
    uint32_t raw_pc;
    int32_t exception_type;
    uint32_t handoff_owner;
    uint32_t handoff_wait_seq;
    uint32_t handoff_done_seq;
    uint32_t handoff_wait_attempts;
    int32_t handoff_wait_result;
    uint32_t guard_seq;
    int32_t guard_result;
    uint32_t session_seq;
    int32_t session_claimable;
    uint32_t protocol_seq;
    int32_t protocol_result;
    uint32_t lock_seq;
    int32_t lock_result;
    uint32_t predecessor_seq;
    int32_t predecessor_reason;
    int32_t predecessor_invoked;
    uint32_t publish_seq;
    uint32_t classified_pc;
    int32_t signal;
    int32_t synthetic_trap;
    int32_t breakpoint_match;
    uint32_t stop_begin_seq;
    int32_t stop_begin_result;
    uint32_t stopped_operation_seq;
    int32_t stopped_operation_result;
    uint32_t main_loop_seq;
    uint32_t packet_wait_seq;
    uint32_t socket_poll_seq;
    uint32_t socket_wake_seq;
    int32_t socket_wake_result;
    uint32_t packet_ready_seq;
    uint32_t status_query_seq;
    uint32_t reply_attempt_seq;
    uint32_t reply_socket_poll_seq;
    uint32_t reply_socket_wake_seq;
    int32_t reply_socket_wake_result;
    uint32_t reply_result_seq;
    int32_t reply_result;
    uint32_t exit_seq;
};

enum uvdb_monitor_thread_flag {
    UVDB_MONITOR_THREAD_STOPPED = 1u << 0,
    UVDB_MONITOR_THREAD_GENERAL = 1u << 1,
    UVDB_MONITOR_THREAD_RESUME = 1u << 2,
    UVDB_MONITOR_THREAD_EXCEPTION = 1u << 3,
};

struct uvdb_monitor_status {
    enum uvdb_monitor_state state;
    uint32_t port;
    uint32_t packet_size;
    uint32_t thread_count;
    uint32_t software_breakpoint_count;
    int32_t stopped_thread;
    int32_t general_thread;
    int32_t resume_thread;
    int32_t exception_thread;
    int32_t stop_signal;
    int target_stopped;
    int no_ack_mode;

    int kernel_compiled;
    int kernel_status_available;
    int kernel_compatible;
    uint32_t kernel_abi;
    uint32_t kernel_capabilities;
    uint32_t kernel_max_threads;
    int stop_session_active;
    int stop_session_failed;
    int vfp_reads_enabled;

    int stop_injection_available;
    enum uvdb_monitor_stop_injection_operation
        stop_injection_pending_operation;
    uint32_t stop_injection_pending_token;
    uint32_t stop_injection_pending_generation;
    enum uvdb_monitor_stop_injection_operation
        stop_injection_last_operation;
    enum uvdb_monitor_stop_injection_outcome
        stop_injection_last_outcome;
    int32_t stop_injection_last_result;
    uint32_t stop_injection_last_token;
    uint32_t stop_injection_last_generation;

    int last_fault_available;
    int32_t last_fault_type;
    uint32_t last_fault_status;
    uint32_t last_fault_address;
    uint32_t last_fault_pc;
    const struct uvdb_monitor_stop_trace* stop_traces;
    size_t stop_trace_count;
    uint32_t stop_trace_dropped;
};

struct uvdb_monitor_thread {
    int32_t id;
    uint32_t flags;
    char name[UVDB_MONITOR_THREAD_NAME_MAX];
};

struct uvdb_monitor_segment {
    uint32_t address;
    uint32_t memory_size;
    uint32_t permissions;
    uint32_t index;
};

struct uvdb_monitor_module {
    uint32_t id;
    char name[UVDB_MONITOR_MODULE_NAME_MAX];
    struct uvdb_monitor_segment segments[UVDB_MONITOR_MAX_SEGMENTS];
    size_t segment_count;
};

struct uvdb_monitor_console {
    int available;
    uint32_t session_open;
    uint32_t session_generation;
    uint32_t queued_records;
    uint32_t queued_bytes;
    uint32_t sessions_opened;
    uint32_t reconnects;
    uint32_t accepted_records;
    uint32_t accepted_bytes;
    uint32_t sent_records;
    uint32_t sent_bytes;
    uint32_t dropped_disconnected_records;
    uint32_t dropped_disconnected_bytes;
    uint32_t dropped_contention_records;
    uint32_t dropped_contention_bytes;
    uint32_t dropped_full_records;
    uint32_t dropped_full_bytes;
    uint32_t dropped_stale_records;
    uint32_t dropped_stale_bytes;
    uint32_t no_ack_mode;
    uint32_t transport_failed;
    uint32_t frames_sent;
    uint32_t frame_bytes_sent;
    uint32_t would_block;
    uint32_t commit_busy;
    uint32_t partial_writes;
    uint32_t hard_errors;
    uint32_t session_errors;
    int32_t last_native_error;
};

struct uvdb_monitor_framebuffer {
    int32_t query_result;
    uint32_t address;
    uint32_t pitch;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
};

struct uvdb_monitor_display {
    int available;
    int32_t primary_head;
    int32_t vcount;
    int32_t refresh_query_result;
    uint32_t refresh_millihz;
    int32_t maximum_query_result;
    uint32_t maximum_width;
    uint32_t maximum_height;
    struct uvdb_monitor_framebuffer immediate;
    struct uvdb_monitor_framebuffer next_frame;
};

struct uvdb_monitor_snapshot {
    struct uvdb_monitor_status status;
    const struct uvdb_monitor_thread* threads;
    size_t thread_count;
    const struct uvdb_monitor_module* modules;
    size_t module_count;
    size_t module_reported_count;
    size_t module_scanned_count;
    size_t module_skipped_count;
    int module_query_result;
    struct uvdb_monitor_console console;
    struct uvdb_monitor_display display;
};

/* Parse one complete qRcmd packet. The command bytes are hex encoded after the
 * required comma. Only the exact read-only registry is accepted; no command
 * text is executed or forwarded to an interpreter. */
int uvdb_monitor_parse_qrcmd(
    const char* packet,
    size_t packet_size,
    enum uvdb_monitor_command* command);

/* Match one exact command after applying the same bounded hex decoding and
 * surrounding-whitespace rules as uvdb_monitor_parse_qrcmd(). */
int uvdb_monitor_qrcmd_text_equals(
    const char* packet,
    size_t packet_size,
    const char* expected);

/* Render one command's plain-text result. output_size is the actual number of
 * bytes written and the result reports deliberate line-safe truncation. The
 * output is not NUL terminated; the RSP owner hex-encodes it for qRcmd. */
int uvdb_monitor_render(
    enum uvdb_monitor_command command,
    const struct uvdb_monitor_snapshot* snapshot,
    char* output,
    size_t capacity,
    size_t* output_size);

/* Convert a nonnegative finite IEEE-754 binary32 refresh rate into rounded
 * millihertz without executing floating-point instructions in debugger code. */
int uvdb_monitor_ieee754_to_millihz(
    uint32_t bits,
    uint32_t* millihz);

void uvdb_monitor_display_stop_reset(
    struct uvdb_monitor_display_stop* stop);

int uvdb_monitor_display_stop_begin(
    struct uvdb_monitor_display_stop* stop,
    uint32_t generation);

uint32_t uvdb_monitor_display_stop_pending_generation(
    const struct uvdb_monitor_display_stop* stop);

int uvdb_monitor_display_stop_arm(
    struct uvdb_monitor_display_stop* stop,
    uint32_t generation);

enum uvdb_monitor_display_stop_action uvdb_monitor_display_stop_on_exception(
    struct uvdb_monitor_display_stop* stop,
    int is_initial_stop_trap);

/* A display snapshot is intentionally unavailable until it belongs to the
 * exact currently connected socket generation. */
int uvdb_monitor_display_generation_is_current(
    uint32_t sample_generation,
    int active_socket,
    uint32_t active_generation);
