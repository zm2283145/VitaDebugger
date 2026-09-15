#ifndef VITAPROFILER_TCP_VITA_H
#define VITAPROFILER_TCP_VITA_H

#include "vitaprofiler_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VP_VITA_TCP_DEFAULT_CONNECT_TIMEOUT_MS UINT32_C(5000)
#define VP_VITA_TCP_DEFAULT_SEND_TIMEOUT_MS UINT32_C(2000)
#define VP_VITA_TCP_DEFAULT_WAIT_SLICE_MS UINT32_C(100)
#define VP_VITA_TCP_DEFAULT_MAX_CONNECT_WAITS UINT32_C(64)
#define VP_VITA_TCP_DEFAULT_MAX_SEND_WAITS UINT32_C(64)
#define VP_VITA_TCP_DEFAULT_MAX_SEND_CALLS UINT32_C(1024)
#define VP_VITA_TCP_DEFAULT_SEND_CHUNK_BYTES ((size_t)16384u)

enum vp_vita_tcp_io_result {
    VP_VITA_TCP_IO_OK = 0,
    VP_VITA_TCP_IO_WOULD_BLOCK = 1,
    VP_VITA_TCP_IO_TIMEOUT = 2,
    VP_VITA_TCP_IO_ERROR = -1,
};

enum vp_vita_tcp_sink_state {
    VP_VITA_TCP_SINK_UNINITIALIZED = 0,
    VP_VITA_TCP_SINK_READY = 1,
    VP_VITA_TCP_SINK_CONNECTED = 2,
    VP_VITA_TCP_SINK_CLOSED = 3,
    VP_VITA_TCP_SINK_FAILED = 4,
};

enum vp_vita_tcp_failure {
    VP_VITA_TCP_FAILURE_NONE = 0,
    VP_VITA_TCP_FAILURE_CLOCK = 1,
    VP_VITA_TCP_FAILURE_OPEN = 2,
    VP_VITA_TCP_FAILURE_CONNECT = 3,
    VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT = 4,
    VP_VITA_TCP_FAILURE_CONNECT_LIMIT = 5,
    VP_VITA_TCP_FAILURE_SEND = 6,
    VP_VITA_TCP_FAILURE_SEND_TIMEOUT = 7,
    VP_VITA_TCP_FAILURE_SEND_LIMIT = 8,
    VP_VITA_TCP_FAILURE_IO_CONTRACT = 9,
    VP_VITA_TCP_FAILURE_SHUTDOWN = 10,
    VP_VITA_TCP_FAILURE_CLOSE = 11,
};

struct vp_vita_tcp_endpoint {
    uint8_t ipv4[4];
    uint16_t port;
    uint16_t reserved;
};

/* All callbacks except wait_writable must be nonblocking. wait_writable must
 * return within timeout_ms. The adapter resamples now_ms after every callback
 * and rejects a late success, but it cannot preempt a callback which violates
 * that contract. On open failure, socket_out may still contain a descriptor
 * which the adapter will close. send must report 1..size bytes with OK and
 * zero bytes with WOULD_BLOCK/ERROR. */
struct vp_vita_tcp_socket_ops {
    uint64_t (*now_ms)(void* user);
    int (*open)(void* user, int* socket_out, int* native_error);
    int (*connect_start)(void* user, int socket,
                         const struct vp_vita_tcp_endpoint* endpoint,
                         int* native_error);
    int (*connect_finish)(void* user, int socket, int* native_error);
    int (*wait_writable)(void* user, int socket, uint32_t timeout_ms,
                         int* native_error);
    int (*send)(void* user, int socket, const uint8_t* data, size_t size,
                size_t* bytes_sent, int* native_error);
    int (*shutdown_write)(void* user, int socket, int* native_error);
    int (*close)(void* user, int socket, int* native_error);
};

struct vp_vita_tcp_sink_config {
    struct vp_vita_tcp_endpoint endpoint;
    struct vp_vita_tcp_socket_ops ops;
    void* ops_user;
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t wait_slice_ms;
    uint32_t max_connect_waits;
    uint32_t max_send_waits;
    uint32_t max_send_calls;
    size_t max_send_chunk_bytes;
};

/* Public only for caller-owned/static allocation. Zero-initialize before the
 * first init and treat fields as private thereafter. One instance owns at most
 * one socket but never owns SceNet's module or global initialization. */
struct vp_vita_tcp_sink {
    struct vp_vita_tcp_socket_ops ops;
    void* ops_user;
    struct vp_vita_tcp_endpoint endpoint;
    uint64_t bytes_sent;
    uint64_t write_calls;
    uint32_t connect_attempts;
    uint32_t connect_waits;
    uint32_t send_calls;
    uint32_t send_waits;
    uint32_t partial_sends;
    uint32_t would_blocks;
    uint32_t failures;
    uint32_t shutdown_errors;
    uint32_t close_attempts;
    uint32_t close_errors;
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t wait_slice_ms;
    uint32_t max_connect_waits;
    uint32_t max_send_waits;
    uint32_t max_send_calls;
    size_t max_send_chunk_bytes;
    int socket;
    int32_t last_native_error;
    uint32_t state;
    uint32_t failure;
    uint32_t initialized;
};

struct vp_vita_tcp_sink_stats {
    uint64_t bytes_sent;
    uint64_t write_calls;
    uint32_t connect_attempts;
    uint32_t connect_waits;
    uint32_t send_calls;
    uint32_t send_waits;
    uint32_t partial_sends;
    uint32_t would_blocks;
    uint32_t failures;
    uint32_t shutdown_errors;
    uint32_t close_attempts;
    uint32_t close_errors;
    int32_t last_native_error;
    uint32_t state;
    uint32_t failure;
};

void vp_vita_tcp_sink_config_init(struct vp_vita_tcp_sink_config* config);
int vp_vita_tcp_sink_init(struct vp_vita_tcp_sink* sink,
                          const struct vp_vita_tcp_sink_config* config);
int vp_vita_tcp_sink_connect(struct vp_vita_tcp_sink* sink);

/* Pass this directly as vp_stream_writer_config.write and pass the sink as
 * write_user. It returns zero only after all bytes have been sent. Any
 * ambiguous/partial failure permanently fails and closes the connection. */
int vp_vita_tcp_sink_write(void* user, const uint8_t* data, size_t size);

/* Idempotent after the descriptor is known closed. A close failure retains
 * the descriptor so a later call can retry. A previously failed capture stays
 * FAILED in stats even after resource cleanup succeeds. */
int vp_vita_tcp_sink_close(struct vp_vita_tcp_sink* sink);
int vp_vita_tcp_sink_get_stats(const struct vp_vita_tcp_sink* sink,
                               struct vp_vita_tcp_sink_stats* stats);

#if defined(__vita__) || defined(VP_VITA_TCP_SCE_NET_HOST_TEST)
#define VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER {0}

/* Caller-owned backend for the public SceNet implementation. This initializer
 * only fills callbacks. It does not load SCE_SYSMODULE_NET and never calls
 * sceNetInit()/sceNetTerm(); the application must own that lifecycle.
 * Initialize storage with VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER (or zero it)
 * before the first call. Later calls return BUSY while any socket/epoll cleanup
 * obligation remains and are permitted after a successful close. Treat every
 * field as private. */
struct vp_vita_tcp_sce_net_backend {
    int socket;
    int epoll;
    int clock_failed;
    int clock_native_error;
    uint32_t owned_resources;
    uint32_t initialized;
};

int vp_vita_tcp_sce_net_ops_init(
    struct vp_vita_tcp_sce_net_backend* backend,
    struct vp_vita_tcp_socket_ops* ops);
#endif

#ifdef __cplusplus
}
#endif

#endif
