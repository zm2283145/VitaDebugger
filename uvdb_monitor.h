#pragma once

#include <stddef.h>
#include <stdint.h>

#define UVDB_MONITOR_COMMAND_TEXT_MAX 32u
#define UVDB_MONITOR_THREAD_NAME_MAX 32u
#define UVDB_MONITOR_MODULE_NAME_MAX 32u
#define UVDB_MONITOR_MAX_THREADS 64u
#define UVDB_MONITOR_MAX_MODULES 128u
#define UVDB_MONITOR_MAX_SEGMENTS 4u

enum uvdb_monitor_command {
    UVDB_MONITOR_COMMAND_NONE = 0,
    UVDB_MONITOR_COMMAND_HELP,
    UVDB_MONITOR_COMMAND_STATUS,
    UVDB_MONITOR_COMMAND_THREADS,
    UVDB_MONITOR_COMMAND_MODULES,
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

    int last_fault_available;
    int32_t last_fault_type;
    uint32_t last_fault_status;
    uint32_t last_fault_address;
    uint32_t last_fault_pc;
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
};

/* Parse one complete qRcmd packet. The command bytes are hex encoded after the
 * required comma. Only the exact read-only registry is accepted; no command
 * text is executed or forwarded to an interpreter. */
int uvdb_monitor_parse_qrcmd(
    const char* packet,
    size_t packet_size,
    enum uvdb_monitor_command* command);

/* Render one command's plain-text result. output_size is the actual number of
 * bytes written and the result reports deliberate line-safe truncation. The
 * output is not NUL terminated; the RSP owner hex-encodes it for qRcmd. */
int uvdb_monitor_render(
    enum uvdb_monitor_command command,
    const struct uvdb_monitor_snapshot* snapshot,
    char* output,
    size_t capacity,
    size_t* output_size);
