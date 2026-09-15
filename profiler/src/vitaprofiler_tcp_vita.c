#include "vitaprofiler_tcp_vita.h"

#include <limits.h>
#include <string.h>

#define VP_VITA_TCP_SINK_MAGIC UINT32_C(0x56505443)
#define VP_VITA_TCP_INVALID_SOCKET (-1)
#define VP_VITA_TCP_US_PER_MS UINT32_C(1000)

#if defined(__vita__) || defined(VP_VITA_TCP_HOST_TEST)
/* SceNet epoll accepts a signed microsecond timeout. Keep the public adapter
 * contract in milliseconds and saturate before converting so large, valid
 * caller timeouts cannot wrap through the production backend. */
static int vp_tcp_sce_net_timeout_us(uint32_t timeout_ms)
{
    if (timeout_ms > (uint32_t)INT_MAX / VP_VITA_TCP_US_PER_MS)
        return INT_MAX;
    return (int)(timeout_ms * VP_VITA_TCP_US_PER_MS);
}

#if defined(VP_VITA_TCP_HOST_TEST)
int vp_vita_tcp_test_sce_net_timeout_us(uint32_t timeout_ms)
{
    return vp_tcp_sce_net_timeout_us(timeout_ms);
}
#endif
#endif

static void vp_tcp_increment_u32(uint32_t* value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

static void vp_tcp_add_u64(uint64_t* value, size_t amount)
{
    const uint64_t add = (uint64_t)amount;
    if (UINT64_MAX - *value < add)
        *value = UINT64_MAX;
    else
        *value += add;
}

static int vp_tcp_is_valid(const struct vp_vita_tcp_sink* sink)
{
    return sink != NULL && sink->initialized == VP_VITA_TCP_SINK_MAGIC &&
           sink->ops.now_ms != NULL && sink->ops.open != NULL &&
           sink->ops.connect_start != NULL &&
           sink->ops.connect_finish != NULL &&
           sink->ops.wait_writable != NULL && sink->ops.send != NULL &&
           sink->ops.shutdown_write != NULL && sink->ops.close != NULL;
}

static int vp_tcp_endpoint_is_valid(
    const struct vp_vita_tcp_endpoint* endpoint)
{
    return endpoint->port != 0u &&
           (endpoint->ipv4[0] != 0u || endpoint->ipv4[1] != 0u ||
            endpoint->ipv4[2] != 0u || endpoint->ipv4[3] != 0u);
}

/* Returns zero with a usable remaining interval, one at/after the deadline,
 * and minus one if the supplied monotonic clock regressed. */
static int vp_tcp_deadline_remaining(struct vp_vita_tcp_sink* sink,
                                     uint64_t start_ms,
                                     uint32_t timeout_ms,
                                     uint32_t* remaining_ms)
{
    const uint64_t now_ms = sink->ops.now_ms(sink->ops_user);
    uint64_t elapsed;

    if (now_ms < start_ms)
        return -1;
    elapsed = now_ms - start_ms;
    if (elapsed >= (uint64_t)timeout_ms) {
        *remaining_ms = 0u;
        return 1;
    }
    *remaining_ms = (uint32_t)((uint64_t)timeout_ms - elapsed);
    return 0;
}

static uint32_t vp_tcp_wait_interval(const struct vp_vita_tcp_sink* sink,
                                     uint32_t remaining_ms)
{
    return remaining_ms < sink->wait_slice_ms ? remaining_ms
                                               : sink->wait_slice_ms;
}

static void vp_tcp_cleanup_socket(struct vp_vita_tcp_sink* sink,
                                  int shutdown_first)
{
    int native_error = 0;
    int result;

    if (sink->socket < 0)
        return;
    if (shutdown_first) {
        result = sink->ops.shutdown_write(sink->ops_user, sink->socket,
                                          &native_error);
        if (result != VP_VITA_TCP_IO_OK) {
            vp_tcp_increment_u32(&sink->shutdown_errors);
            if (sink->last_native_error == 0)
                sink->last_native_error = native_error;
        }
    }
    native_error = 0;
    vp_tcp_increment_u32(&sink->close_attempts);
    result = sink->ops.close(sink->ops_user, sink->socket, &native_error);
    if (result == VP_VITA_TCP_IO_OK) {
        sink->socket = VP_VITA_TCP_INVALID_SOCKET;
    } else {
        vp_tcp_increment_u32(&sink->close_errors);
        if (sink->last_native_error == 0)
            sink->last_native_error = native_error;
    }
}

static int vp_tcp_fail(struct vp_vita_tcp_sink* sink, uint32_t failure,
                       int native_error, int was_connected)
{
    if (sink->failure == VP_VITA_TCP_FAILURE_NONE)
        sink->failure = failure;
    if (native_error != 0)
        sink->last_native_error = native_error;
    vp_tcp_increment_u32(&sink->failures);
    sink->state = VP_VITA_TCP_SINK_FAILED;
    vp_tcp_cleanup_socket(sink, was_connected);
    return VP_ERROR_IO;
}

void vp_vita_tcp_sink_config_init(struct vp_vita_tcp_sink_config* config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->connect_timeout_ms = VP_VITA_TCP_DEFAULT_CONNECT_TIMEOUT_MS;
    config->send_timeout_ms = VP_VITA_TCP_DEFAULT_SEND_TIMEOUT_MS;
    config->wait_slice_ms = VP_VITA_TCP_DEFAULT_WAIT_SLICE_MS;
    config->max_connect_waits = VP_VITA_TCP_DEFAULT_MAX_CONNECT_WAITS;
    config->max_send_waits = VP_VITA_TCP_DEFAULT_MAX_SEND_WAITS;
    config->max_send_calls = VP_VITA_TCP_DEFAULT_MAX_SEND_CALLS;
    config->max_send_chunk_bytes = VP_VITA_TCP_DEFAULT_SEND_CHUNK_BYTES;
}

int vp_vita_tcp_sink_init(struct vp_vita_tcp_sink* sink,
                          const struct vp_vita_tcp_sink_config* config)
{
    if (sink == NULL || config == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (sink->initialized == VP_VITA_TCP_SINK_MAGIC && sink->socket >= 0)
        return VP_ERROR_BUSY;
    if (!vp_tcp_endpoint_is_valid(&config->endpoint) ||
        config->ops.now_ms == NULL || config->ops.open == NULL ||
        config->ops.connect_start == NULL ||
        config->ops.connect_finish == NULL ||
        config->ops.wait_writable == NULL || config->ops.send == NULL ||
        config->ops.shutdown_write == NULL || config->ops.close == NULL ||
        config->connect_timeout_ms == 0u ||
        config->send_timeout_ms == 0u || config->wait_slice_ms == 0u ||
        config->wait_slice_ms > (uint32_t)INT_MAX ||
        config->max_connect_waits == 0u ||
        config->max_send_waits == 0u || config->max_send_calls == 0u ||
        config->max_send_chunk_bytes == 0u ||
        config->max_send_chunk_bytes > (size_t)UINT_MAX)
        return VP_ERROR_INVALID_ARGUMENT;

    memset(sink, 0, sizeof(*sink));
    sink->ops = config->ops;
    sink->ops_user = config->ops_user;
    sink->endpoint = config->endpoint;
    sink->connect_timeout_ms = config->connect_timeout_ms;
    sink->send_timeout_ms = config->send_timeout_ms;
    sink->wait_slice_ms = config->wait_slice_ms;
    sink->max_connect_waits = config->max_connect_waits;
    sink->max_send_waits = config->max_send_waits;
    sink->max_send_calls = config->max_send_calls;
    sink->max_send_chunk_bytes = config->max_send_chunk_bytes;
    sink->socket = VP_VITA_TCP_INVALID_SOCKET;
    sink->state = VP_VITA_TCP_SINK_READY;
    sink->initialized = VP_VITA_TCP_SINK_MAGIC;
    return VP_RESULT_OK;
}

int vp_vita_tcp_sink_connect(struct vp_vita_tcp_sink* sink)
{
    uint64_t start_ms;
    uint32_t remaining_ms;
    uint32_t waits = 0u;
    int native_error = 0;
    int result;
    int deadline;

    if (!vp_tcp_is_valid(sink))
        return VP_ERROR_NOT_INITIALIZED;
    if (sink->state == VP_VITA_TCP_SINK_FAILED)
        return VP_ERROR_IO;
    if (sink->state != VP_VITA_TCP_SINK_READY)
        return VP_ERROR_STATE;

    start_ms = sink->ops.now_ms(sink->ops_user);
    vp_tcp_increment_u32(&sink->connect_attempts);
    result = sink->ops.open(sink->ops_user, &sink->socket, &native_error);
    if (result != VP_VITA_TCP_IO_OK)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_OPEN, native_error, 0);
    if (sink->socket < 0)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_IO_CONTRACT, 0, 0);

    deadline = vp_tcp_deadline_remaining(
        sink, start_ms, sink->connect_timeout_ms, &remaining_ms);
    if (deadline < 0)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0, 0);
    if (deadline > 0)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT, 0, 0);

    native_error = 0;
    result = sink->ops.connect_start(sink->ops_user, sink->socket,
                                     &sink->endpoint, &native_error);
    deadline = vp_tcp_deadline_remaining(
        sink, start_ms, sink->connect_timeout_ms, &remaining_ms);
    if (deadline < 0)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0,
                           result == VP_VITA_TCP_IO_OK);
    if (deadline > 0)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT,
                           native_error, result == VP_VITA_TCP_IO_OK);
    if (result == VP_VITA_TCP_IO_OK) {
        sink->state = VP_VITA_TCP_SINK_CONNECTED;
        return VP_RESULT_OK;
    }
    if (result != VP_VITA_TCP_IO_WOULD_BLOCK)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT, native_error,
                           0);
    vp_tcp_increment_u32(&sink->would_blocks);

    for (;;) {
        if (waits >= sink->max_connect_waits)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_LIMIT, 0,
                               0);
        deadline = vp_tcp_deadline_remaining(
            sink, start_ms, sink->connect_timeout_ms, &remaining_ms);
        if (deadline < 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0, 0);
        if (deadline > 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT, 0,
                               0);

        native_error = 0;
        ++waits;
        vp_tcp_increment_u32(&sink->connect_waits);
        result = sink->ops.wait_writable(
            sink->ops_user, sink->socket,
            vp_tcp_wait_interval(sink, remaining_ms), &native_error);
        deadline = vp_tcp_deadline_remaining(
            sink, start_ms, sink->connect_timeout_ms, &remaining_ms);
        if (deadline < 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0, 0);
        if (deadline > 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT,
                               native_error, 0);
        if (result == VP_VITA_TCP_IO_TIMEOUT ||
            result == VP_VITA_TCP_IO_WOULD_BLOCK)
            continue;
        if (result != VP_VITA_TCP_IO_OK)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT,
                               native_error, 0);

        native_error = 0;
        result = sink->ops.connect_finish(sink->ops_user, sink->socket,
                                          &native_error);
        deadline = vp_tcp_deadline_remaining(
            sink, start_ms, sink->connect_timeout_ms, &remaining_ms);
        if (deadline < 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0,
                               result == VP_VITA_TCP_IO_OK);
        if (deadline > 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT,
                               native_error,
                               result == VP_VITA_TCP_IO_OK);
        if (result == VP_VITA_TCP_IO_OK) {
            sink->state = VP_VITA_TCP_SINK_CONNECTED;
            return VP_RESULT_OK;
        }
        if (result != VP_VITA_TCP_IO_WOULD_BLOCK)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CONNECT,
                               native_error, 0);
        vp_tcp_increment_u32(&sink->would_blocks);
    }
}

int vp_vita_tcp_sink_write(void* user, const uint8_t* data, size_t size)
{
    struct vp_vita_tcp_sink* sink = (struct vp_vita_tcp_sink*)user;
    uint64_t start_ms;
    size_t offset = 0u;
    uint32_t local_send_calls = 0u;
    uint32_t local_send_waits = 0u;

    if (!vp_tcp_is_valid(sink))
        return VP_ERROR_NOT_INITIALIZED;
    if (sink->state == VP_VITA_TCP_SINK_FAILED)
        return VP_ERROR_IO;
    if (sink->state != VP_VITA_TCP_SINK_CONNECTED)
        return VP_ERROR_STATE;
    vp_tcp_add_u64(&sink->write_calls, 1u);
    if (size == 0u)
        return VP_RESULT_OK;
    if (data == NULL)
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_IO_CONTRACT, 0, 1);

    start_ms = sink->ops.now_ms(sink->ops_user);
    while (offset < size) {
        uint32_t remaining_ms;
        size_t chunk = size - offset;
        size_t sent = 0u;
        int native_error = 0;
        int result;
        int deadline;

        if (local_send_calls >= sink->max_send_calls)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND_LIMIT, 0, 1);
        if (chunk > sink->max_send_chunk_bytes)
            chunk = sink->max_send_chunk_bytes;
        ++local_send_calls;
        vp_tcp_increment_u32(&sink->send_calls);
        result = sink->ops.send(sink->ops_user, sink->socket, data + offset,
                                chunk, &sent, &native_error);

        if (result == VP_VITA_TCP_IO_OK) {
            if (sent == 0u || sent > chunk)
                return vp_tcp_fail(sink,
                                   VP_VITA_TCP_FAILURE_IO_CONTRACT,
                                   native_error, 1);
            if (sent < chunk)
                vp_tcp_increment_u32(&sink->partial_sends);
            offset += sent;
            vp_tcp_add_u64(&sink->bytes_sent, sent);
        } else if (sent != 0u) {
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_IO_CONTRACT,
                               native_error, 1);
        }

        deadline = vp_tcp_deadline_remaining(
            sink, start_ms, sink->send_timeout_ms, &remaining_ms);
        if (deadline < 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0, 1);
        if (deadline > 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND_TIMEOUT,
                               native_error, 1);
        if (result == VP_VITA_TCP_IO_OK)
            continue;
        if (result != VP_VITA_TCP_IO_WOULD_BLOCK)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND, native_error,
                               1);
        vp_tcp_increment_u32(&sink->would_blocks);

        if (local_send_waits >= sink->max_send_waits)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND_LIMIT, 0, 1);
        ++local_send_waits;
        vp_tcp_increment_u32(&sink->send_waits);
        native_error = 0;
        result = sink->ops.wait_writable(
            sink->ops_user, sink->socket,
            vp_tcp_wait_interval(sink, remaining_ms), &native_error);
        deadline = vp_tcp_deadline_remaining(
            sink, start_ms, sink->send_timeout_ms, &remaining_ms);
        if (deadline < 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_CLOCK, 0, 1);
        if (deadline > 0)
            return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND_TIMEOUT,
                               native_error, 1);
        if (result == VP_VITA_TCP_IO_OK ||
            result == VP_VITA_TCP_IO_TIMEOUT ||
            result == VP_VITA_TCP_IO_WOULD_BLOCK)
            continue;
        return vp_tcp_fail(sink, VP_VITA_TCP_FAILURE_SEND, native_error, 1);
    }
    return VP_RESULT_OK;
}

int vp_vita_tcp_sink_close(struct vp_vita_tcp_sink* sink)
{
    int had_failure;
    int shutdown_failed = 0;
    int native_error = 0;
    int result;

    if (!vp_tcp_is_valid(sink))
        return VP_ERROR_NOT_INITIALIZED;
    had_failure = sink->state == VP_VITA_TCP_SINK_FAILED;
    if (sink->socket < 0) {
        if (!had_failure)
            sink->state = VP_VITA_TCP_SINK_CLOSED;
        return VP_RESULT_OK;
    }

    if (sink->state == VP_VITA_TCP_SINK_CONNECTED) {
        result = sink->ops.shutdown_write(sink->ops_user, sink->socket,
                                          &native_error);
        if (result != VP_VITA_TCP_IO_OK) {
            shutdown_failed = 1;
            vp_tcp_increment_u32(&sink->shutdown_errors);
            sink->last_native_error = native_error;
            if (sink->failure == VP_VITA_TCP_FAILURE_NONE)
                sink->failure = VP_VITA_TCP_FAILURE_SHUTDOWN;
        }
    }

    native_error = 0;
    vp_tcp_increment_u32(&sink->close_attempts);
    result = sink->ops.close(sink->ops_user, sink->socket, &native_error);
    if (result != VP_VITA_TCP_IO_OK) {
        vp_tcp_increment_u32(&sink->close_errors);
        vp_tcp_increment_u32(&sink->failures);
        sink->last_native_error = native_error;
        if (sink->failure == VP_VITA_TCP_FAILURE_NONE)
            sink->failure = VP_VITA_TCP_FAILURE_CLOSE;
        sink->state = VP_VITA_TCP_SINK_FAILED;
        return VP_ERROR_IO;
    }
    sink->socket = VP_VITA_TCP_INVALID_SOCKET;
    if (shutdown_failed) {
        vp_tcp_increment_u32(&sink->failures);
        sink->state = VP_VITA_TCP_SINK_FAILED;
        return VP_ERROR_IO;
    }
    sink->state = had_failure ? VP_VITA_TCP_SINK_FAILED
                              : VP_VITA_TCP_SINK_CLOSED;
    return VP_RESULT_OK;
}

int vp_vita_tcp_sink_get_stats(const struct vp_vita_tcp_sink* sink,
                               struct vp_vita_tcp_sink_stats* stats)
{
    if (!vp_tcp_is_valid(sink))
        return VP_ERROR_NOT_INITIALIZED;
    if (stats == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    stats->bytes_sent = sink->bytes_sent;
    stats->write_calls = sink->write_calls;
    stats->connect_attempts = sink->connect_attempts;
    stats->connect_waits = sink->connect_waits;
    stats->send_calls = sink->send_calls;
    stats->send_waits = sink->send_waits;
    stats->partial_sends = sink->partial_sends;
    stats->would_blocks = sink->would_blocks;
    stats->failures = sink->failures;
    stats->shutdown_errors = sink->shutdown_errors;
    stats->close_attempts = sink->close_attempts;
    stats->close_errors = sink->close_errors;
    stats->last_native_error = sink->last_native_error;
    stats->state = sink->state;
    stats->failure = sink->failure;
    return VP_RESULT_OK;
}

#if defined(__vita__) || defined(VP_VITA_TCP_SCE_NET_HOST_TEST)

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#else
#include "../tests/vita_tcp_sce_net_shim.h"
#endif

#define VP_SCE_NET_BACKEND_MAGIC UINT32_C(0x56544231)
#define VP_SCE_NET_OWNS_SOCKET UINT32_C(0x00000001)
#define VP_SCE_NET_OWNS_EPOLL UINT32_C(0x00000002)

static int vp_sce_net_native_error(int result)
{
    int* error_location;

    if (result != -1)
        return result;
    error_location = sceNetErrnoLoc();
    return error_location != NULL ? *error_location : result;
}

static int vp_sce_net_error_is(int result, int native_error,
                               uint32_t encoded, int plain)
{
    return (uint32_t)result == encoded || (uint32_t)native_error == encoded ||
           native_error == plain;
}

static uint64_t vp_sce_net_now_ms(void* user)
{
    struct vp_vita_tcp_sce_net_backend* backend =
        (struct vp_vita_tcp_sce_net_backend*)user;
    const int64_t now_us = (int64_t)sceKernelGetProcessTimeWide();

    /* Keep a platform error-shaped value from becoming a huge unsigned
     * timestamp. open observes the sticky error if this was the initial clock
     * sample; a later error becomes a generic clock regression. */
    if (now_us < 0) {
        backend->clock_failed = 1;
        backend->clock_native_error = (int)now_us;
        return 0u;
    }
    return (uint64_t)now_us / UINT64_C(1000);
}

static int vp_sce_net_open(void* user, int* socket_out, int* native_error)
{
    struct vp_vita_tcp_sce_net_backend* backend =
        (struct vp_vita_tcp_sce_net_backend*)user;
    SceNetEpollEvent event;
    int enabled = 1;
    int result;

    *socket_out = VP_VITA_TCP_INVALID_SOCKET;
    *native_error = 0;
    if (backend->clock_failed) {
        *native_error = backend->clock_native_error;
        return VP_VITA_TCP_IO_ERROR;
    }
    if (backend->owned_resources != 0u || backend->socket >= 0 ||
        backend->epoll >= 0) {
        *native_error = VP_ERROR_STATE;
        return VP_VITA_TCP_IO_ERROR;
    }
    backend->socket = sceNetSocket("VitaProfiler TCP",
                                   SCE_NET_AF_INET,
                                   SCE_NET_SOCK_STREAM,
                                   SCE_NET_IPPROTO_TCP);
    *socket_out = backend->socket;
    if (backend->socket < 0) {
        *native_error = vp_sce_net_native_error(backend->socket);
        return VP_VITA_TCP_IO_ERROR;
    }
    backend->owned_resources = VP_SCE_NET_OWNS_SOCKET;
    result = sceNetSetsockopt(backend->socket, SCE_NET_SOL_SOCKET,
                              SCE_NET_SO_NBIO, &enabled,
                              (unsigned int)sizeof(enabled));
    if (result < 0) {
        *native_error = vp_sce_net_native_error(result);
        return VP_VITA_TCP_IO_ERROR;
    }
    backend->epoll = sceNetEpollCreate("VitaProfiler TCP", 0);
    if (backend->epoll < 0) {
        *native_error = vp_sce_net_native_error(backend->epoll);
        return VP_VITA_TCP_IO_ERROR;
    }
    backend->owned_resources |= VP_SCE_NET_OWNS_EPOLL;
    memset(&event, 0, sizeof(event));
    event.events = SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    event.data.fd = backend->socket;
    result = sceNetEpollControl(backend->epoll, SCE_NET_EPOLL_CTL_ADD,
                                backend->socket, &event);
    if (result < 0) {
        *native_error = vp_sce_net_native_error(result);
        return VP_VITA_TCP_IO_ERROR;
    }
    return VP_VITA_TCP_IO_OK;
}

static int vp_sce_net_connect_start(
    void* user, int socket, const struct vp_vita_tcp_endpoint* endpoint,
    int* native_error)
{
    SceNetSockaddrIn address;
    uint32_t host_address;
    int result;

    (void)user;
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(endpoint->port);
    host_address = ((uint32_t)endpoint->ipv4[0] << 24) |
                   ((uint32_t)endpoint->ipv4[1] << 16) |
                   ((uint32_t)endpoint->ipv4[2] << 8) |
                   (uint32_t)endpoint->ipv4[3];
    address.sin_addr.s_addr = sceNetHtonl(host_address);
    result = sceNetConnect(socket, (const SceNetSockaddr*)&address,
                           (unsigned int)sizeof(address));
    if (result == 0)
        return VP_VITA_TCP_IO_OK;
    *native_error = vp_sce_net_native_error(result);
    if (vp_sce_net_error_is(result, *native_error,
                            SCE_NET_ERROR_EISCONN, SCE_NET_EISCONN))
        return VP_VITA_TCP_IO_OK;
    if (vp_sce_net_error_is(result, *native_error,
                            SCE_NET_ERROR_EINPROGRESS,
                            SCE_NET_EINPROGRESS) ||
        vp_sce_net_error_is(result, *native_error,
                            SCE_NET_ERROR_EALREADY, SCE_NET_EALREADY) ||
        vp_sce_net_error_is(result, *native_error,
                            SCE_NET_ERROR_EAGAIN, SCE_NET_EAGAIN))
        return VP_VITA_TCP_IO_WOULD_BLOCK;
    return VP_VITA_TCP_IO_ERROR;
}

static int vp_sce_net_connect_finish(void* user, int socket,
                                     int* native_error)
{
    int socket_error = 0;
    unsigned int size = (unsigned int)sizeof(socket_error);
    int result;

    (void)user;
    result = sceNetGetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                              &socket_error, &size);
    if (result < 0) {
        *native_error = vp_sce_net_native_error(result);
        return VP_VITA_TCP_IO_ERROR;
    }
    if (socket_error == 0 || socket_error == SCE_NET_EISCONN ||
        (uint32_t)socket_error == (uint32_t)SCE_NET_ERROR_EISCONN)
        return VP_VITA_TCP_IO_OK;
    *native_error = socket_error;
    if (socket_error == SCE_NET_EINPROGRESS ||
        socket_error == SCE_NET_EALREADY || socket_error == SCE_NET_EAGAIN ||
        (uint32_t)socket_error == (uint32_t)SCE_NET_ERROR_EINPROGRESS ||
        (uint32_t)socket_error == (uint32_t)SCE_NET_ERROR_EALREADY ||
        (uint32_t)socket_error == (uint32_t)SCE_NET_ERROR_EAGAIN)
        return VP_VITA_TCP_IO_WOULD_BLOCK;
    return VP_VITA_TCP_IO_ERROR;
}

static int vp_sce_net_wait_writable(void* user, int socket,
                                    uint32_t timeout_ms, int* native_error)
{
    struct vp_vita_tcp_sce_net_backend* backend =
        (struct vp_vita_tcp_sce_net_backend*)user;
    SceNetEpollEvent event;
    int result;

    if (backend->socket != socket || backend->epoll < 0) {
        *native_error = VP_ERROR_STATE;
        return VP_VITA_TCP_IO_ERROR;
    }
    memset(&event, 0, sizeof(event));
    result = sceNetEpollWait(backend->epoll, &event, 1,
                             vp_tcp_sce_net_timeout_us(timeout_ms));
    if (result == 0)
        return VP_VITA_TCP_IO_TIMEOUT;
    if (result < 0) {
        *native_error = vp_sce_net_native_error(result);
        return VP_VITA_TCP_IO_ERROR;
    }
    if (event.data.fd != socket) {
        *native_error = VP_ERROR_STATE;
        return VP_VITA_TCP_IO_ERROR;
    }
    if ((event.events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) != 0u) {
        int socket_error = 0;
        unsigned int size = (unsigned int)sizeof(socket_error);
        result = sceNetGetsockopt(socket, SCE_NET_SOL_SOCKET,
                                  SCE_NET_SO_ERROR, &socket_error, &size);
        if (result < 0) {
            *native_error = vp_sce_net_native_error(result);
            return VP_VITA_TCP_IO_ERROR;
        }
        *native_error = socket_error != 0 ? socket_error : SCE_NET_ENOTCONN;
        return VP_VITA_TCP_IO_ERROR;
    }
    return (event.events & SCE_NET_EPOLLOUT) != 0u
               ? VP_VITA_TCP_IO_OK
               : VP_VITA_TCP_IO_WOULD_BLOCK;
}

static int vp_sce_net_send(void* user, int socket, const uint8_t* data,
                           size_t size, size_t* bytes_sent,
                           int* native_error)
{
    int result;

    (void)user;
    *bytes_sent = 0u;
    result = sceNetSend(socket, data, (unsigned int)size,
                        SCE_NET_MSG_DONTWAIT);
    if (result > 0) {
        *bytes_sent = (size_t)result;
        return VP_VITA_TCP_IO_OK;
    }
    if (result == 0)
        return VP_VITA_TCP_IO_OK;
    *native_error = vp_sce_net_native_error(result);
    if (vp_sce_net_error_is(result, *native_error,
                            SCE_NET_ERROR_EAGAIN, SCE_NET_EAGAIN))
        return VP_VITA_TCP_IO_WOULD_BLOCK;
    return VP_VITA_TCP_IO_ERROR;
}

static int vp_sce_net_shutdown_write(void* user, int socket,
                                     int* native_error)
{
    int result;

    (void)user;
    result = sceNetShutdown(socket, SCE_NET_SHUT_WR);
    if (result < 0) {
        *native_error = vp_sce_net_native_error(result);
        return VP_VITA_TCP_IO_ERROR;
    }
    return VP_VITA_TCP_IO_OK;
}

static int vp_sce_net_close(void* user, int socket, int* native_error)
{
    struct vp_vita_tcp_sce_net_backend* backend =
        (struct vp_vita_tcp_sce_net_backend*)user;
    int result;

    if (backend->socket != socket) {
        *native_error = VP_ERROR_STATE;
        return VP_VITA_TCP_IO_ERROR;
    }
    if ((backend->owned_resources & VP_SCE_NET_OWNS_EPOLL) != 0u) {
        result = sceNetEpollDestroy(backend->epoll);
        if (result < 0) {
            *native_error = vp_sce_net_native_error(result);
            return VP_VITA_TCP_IO_ERROR;
        }
        backend->epoll = -1;
        backend->owned_resources &= ~VP_SCE_NET_OWNS_EPOLL;
    }
    if ((backend->owned_resources & VP_SCE_NET_OWNS_SOCKET) != 0u) {
        result = sceNetSocketClose(socket);
        if (result < 0) {
            *native_error = vp_sce_net_native_error(result);
            return VP_VITA_TCP_IO_ERROR;
        }
        backend->socket = -1;
        backend->owned_resources &= ~VP_SCE_NET_OWNS_SOCKET;
    }
    return VP_VITA_TCP_IO_OK;
}

int vp_vita_tcp_sce_net_ops_init(
    struct vp_vita_tcp_sce_net_backend* backend,
    struct vp_vita_tcp_socket_ops* ops)
{
    if (backend == NULL || ops == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (backend->initialized == VP_SCE_NET_BACKEND_MAGIC &&
        (backend->owned_resources != 0u || backend->socket >= 0 ||
         backend->epoll >= 0))
        return VP_ERROR_BUSY;
    memset(backend, 0, sizeof(*backend));
    backend->socket = -1;
    backend->epoll = -1;
    backend->initialized = VP_SCE_NET_BACKEND_MAGIC;
    memset(ops, 0, sizeof(*ops));
    ops->now_ms = vp_sce_net_now_ms;
    ops->open = vp_sce_net_open;
    ops->connect_start = vp_sce_net_connect_start;
    ops->connect_finish = vp_sce_net_connect_finish;
    ops->wait_writable = vp_sce_net_wait_writable;
    ops->send = vp_sce_net_send;
    ops->shutdown_write = vp_sce_net_shutdown_write;
    ops->close = vp_sce_net_close;
    return VP_RESULT_OK;
}

#endif
