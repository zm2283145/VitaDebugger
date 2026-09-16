#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum uvdb_state {
    UVDB_STATE_IDLE = 0,
    UVDB_STATE_LISTENING,
    UVDB_STATE_CONNECTED,
    UVDB_STATE_ERROR,
};

struct uvdb_config {
    unsigned short port;
    size_t max_packet_buffer;
};

enum uvdb_log_level {
    UVDB_LOG_NONE = 0,
    UVDB_LOG_ERROR = 1,
    UVDB_LOG_INFO = 2,
    UVDB_LOG_DEBUG = 3,
    UVDB_LOG_TRACE = 4,
};

struct uvdb_debugnet_config {
    const char* server_ip;
    unsigned short port;
    enum uvdb_log_level level;
};

struct uvdb_debugnet_stats {
    unsigned int queued;
    unsigned int sent;
    unsigned int dropped;
    unsigned int truncated;
    unsigned int send_errors;
    int last_send_error;
};

// Start an optional DebugNet-compatible UDP log stream. Vita networking must
// already be initialized. The calling thread owns the stream; start it from a
// thread whose lifetime covers logging (normally the application's main
// thread). A single process-wide exit hook synchronously quiesces the sender
// when main returns or exit() is called. Because Vita runs that hook after
// .fini_array destructors, it drops queued data and performs no network calls.
// An exact-owner thread-exit handler remains the forced/SceShell-exit fallback.
// Calls fail cleanly when configuration is invalid or either handler cannot be
// installed; the GDB service is independent. If an initial start fails during
// kernel resource setup, call stop once before retrying so any rare failed
// kernel-handle deletion or socket close can be retried.
int uvdb_debugnet_start(const struct uvdb_debugnet_config* config);

// Queue one bounded message without waiting for network I/O. Returns 0 when
// queued, 1 when filtered, 2 when queued but truncated, or -1 when unavailable
// or full. Datagrams, including the level prefix, are at most 1023 bytes.
int uvdb_debugnet_write(enum uvdb_log_level level, const char* text);
int uvdb_debugnet_printf(enum uvdb_log_level level, const char* format, ...);

// Read counters or stop the sender. Normal stop makes a bounded best-effort
// attempt to send already queued messages before closing the debugger-owned
// UDP socket. Process/owner-exit shutdown drops queued work in favor of prompt
// teardown; another surviving thread may call stop after an owner-thread exit
// to reap handles. Call stop explicitly before application network teardown.
// Stop is retryable when a kernel object deletion or socket close fails.
int uvdb_debugnet_get_stats(struct uvdb_debugnet_stats* stats);
int uvdb_debugnet_stop(void);

// Register the calling thread so it is visible to GDB. Registration is
// cooperative in the application-only library; complete thread suspension and
// foreign-thread register access require a matching opt-in kernel companion.
int uvdb_register_thread(const char* name);
int uvdb_unregister_thread(void);

enum uvdb_exception_type {
    UVDB_EXCEPTION_DATA_ABORT = 0,
    UVDB_EXCEPTION_PREFETCH_ABORT = 1,
    UVDB_EXCEPTION_UNDEFINED_INSTRUCTION = 2,
    UVDB_EXCEPTION_NONE = -1,
};

struct uvdb_fault_info {
    enum uvdb_exception_type exception_type;
    int signal;
    unsigned int fault_status;
    unsigned int fault_address;
    unsigned int pc;
    unsigned int lr;
    unsigned int sp;
};

// Configure the debugger before the first uvdb_enter(). A NULL configuration
// restores the defaults (TCP port 1234, 256 KiB maximum packet buffer).
// Returns 0 on success or -1 if the debugger is already active or the
// configuration is invalid.
int uvdb_configure(const struct uvdb_config* config);

// Query the current debugger state without entering or stopping the debugger.
enum uvdb_state uvdb_get_state(void);

// Copy details for the most recently intercepted exception. Returns 1 when
// details are available, 0 before the first exception, or -1 for NULL output.
int uvdb_get_last_fault(struct uvdb_fault_info* info);

// Start an opt-in debugger service thread. The service keeps accepting clean
// reconnects and converts GDB's Ctrl-C byte into a debugger stop while the
// application is running. Networking must already be initialized.
// Kernel-integrated builds first require the exact companion ABI, thread-control
// capabilities, and inventory size. Returns 0 on success (including when already
// running), or -1 on failure.
int uvdb_start_server(void);

// Stop and delete the debugger service thread. Any active GDB connection is
// closed. Returns 0 on success (including when already stopped), or -1 when
// called from an internal service thread, helper teardown fails, or a pending
// software-breakpoint restoration must remain protected for a later retry.
int uvdb_stop_server(void);

// Terminally stop the service thread, restore debugger-owned handler slots,
// close sockets, and release allocations owned by uvdb. KuBridge does not
// expose dispatcher quiescence after it has copied a callback pointer, so full
// shutdown deliberately cannot be restarted and does not authorize unloading
// a dynamically injected image. Use stop_server/start_server for reconnects.
// Call only from normal application code, never from an exception handler.
void uvdb_shutdown(void);

// uvdb_enter acts as a software breakpoint. On first hit, the program waits for
// GDB to connect; subsequent hits act as software breakpoints. A kernel-enabled
// build fails closed with UVDB_STATE_ERROR before opening a socket when the
// loaded companion ABI or required capabilities do not match.
void uvdb_enter(void);

//gdb exposes a remote syscall api to call some (whitelisted) syscalls on the host
//example:
//  uvdb_remote_syscall("write", 3, 1, "Hello, world!\n", 14); //prints hello world in the debugger prompt
// This legacy path does not own a real saved all-stop context. A File-I/O
// Ctrl-C reply therefore closes the protocol and returns -1 instead of
// reporting a false T02 stop with synthetic zero registers.
int uvdb_remote_syscall(const char* name, int nargs, ... /* int arg1, int arg2, ... */);

// Redirect newlib stdout/stderr into a bounded, nonblocking capture path. Once
// GDB negotiates no-ack mode, captured bytes appear in its console as RSP O
// packets. Writes made without a compatible GDB client, or while buffers are
// saturated, can be dropped instead of stalling the application.
int uvdb_redirect_stdio(void);

// Restore the original newlib stdout/stderr descriptors and join the capture
// helper. Safe to call repeatedly. Stop the debugger server first so an
// asynchronous all-stop cannot suspend the helper during its join; uvdb_shutdown
// performs this ordering automatically. The application must also serialize
// the mapping change with its own concurrent stdio writers.
int uvdb_restore_stdio(void);

#ifdef __cplusplus
}
#endif
