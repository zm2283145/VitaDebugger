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

// Register the calling thread so it is visible to GDB. Registration is
// cooperative in the application-only library; complete thread suspension and
// foreign-thread register access require the planned kernel companion.
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
// Returns 0 on success (including when already running), or -1 on failure.
int uvdb_start_server(void);

// Stop and delete the debugger service thread. Any active GDB connection is
// closed. Returns 0 on success (including when already stopped), or -1 when
// called from the service thread itself.
int uvdb_stop_server(void);

// Stop the service thread, close active/listening sockets, and release
// allocations owned by uvdb.
// Call only from normal application code, never from an exception handler.
void uvdb_shutdown(void);

//uvdb_enter acts as a software breakpoint. on first hit, the program will wait for GDB to connect. on subsequent hits, it will simply act as a software breakpoint
void uvdb_enter(void);

//gdb exposes a remote syscall api to call some (whitelisted) syscalls on the host
//example:
//  uvdb_remote_syscall("write", 3, 1, "Hello, world!\n", 14); //prints hello world in the debugger prompt
int uvdb_remote_syscall(const char* name, int nargs, ... /* int arg1, int arg2, ... */);

//redirects stdout/stderr to go through uvdb_remote_syscall. useful to avoid princesslog & friends
//note: this uses newlib apis, not sce ones
int uvdb_redirect_stdio(void);

#ifdef __cplusplus
}
#endif
