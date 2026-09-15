#include "vitaprofiler_tcp_vita.h"
#include "vita_tcp_sce_net_shim.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

int vp_vita_tcp_test_sce_net_timeout_us(uint32_t timeout_ms);

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);     \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

#define FAKE_STEPS 32u

struct fake_io_step {
    int result;
    size_t bytes;
    int native_error;
    int64_t advance_ms;
};

struct fake_socket {
    uint64_t now_ms;
    int descriptor;
    int open_result;
    int open_native_error;
    int64_t open_advance_ms;
    int connect_start_result;
    int connect_start_native_error;
    int64_t connect_start_advance_ms;
    struct fake_io_step finish[FAKE_STEPS];
    struct fake_io_step wait[FAKE_STEPS];
    struct fake_io_step send[FAKE_STEPS];
    struct fake_io_step close[FAKE_STEPS];
    size_t finish_count;
    size_t wait_count;
    size_t send_count;
    size_t close_count;
    size_t finish_index;
    size_t wait_index;
    size_t send_index;
    size_t close_index;
    int shutdown_result;
    int shutdown_native_error;
    uint32_t open_calls;
    uint32_t connect_start_calls;
    uint32_t finish_calls;
    uint32_t wait_calls;
    uint32_t send_calls;
    uint32_t shutdown_calls;
    uint32_t close_calls;
    uint32_t last_wait_ms;
    size_t largest_send_request;
    struct vp_vita_tcp_endpoint observed_endpoint;
    uint8_t output[8192];
    size_t output_size;
};

static void advance_clock(struct fake_socket* fake, int64_t advance_ms)
{
    if (advance_ms < 0)
        fake->now_ms -= (uint64_t)(-advance_ms);
    else
        fake->now_ms += (uint64_t)advance_ms;
}

static uint64_t fake_now_ms(void* user)
{
    return ((struct fake_socket*)user)->now_ms;
}

static int fake_open(void* user, int* socket_out, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    ++fake->open_calls;
    advance_clock(fake, fake->open_advance_ms);
    *socket_out = fake->descriptor;
    *native_error = fake->open_native_error;
    return fake->open_result;
}

static int fake_connect_start(
    void* user, int socket, const struct vp_vita_tcp_endpoint* endpoint,
    int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    ++fake->connect_start_calls;
    if (socket != fake->descriptor)
        ++failures;
    fake->observed_endpoint = *endpoint;
    advance_clock(fake, fake->connect_start_advance_ms);
    *native_error = fake->connect_start_native_error;
    return fake->connect_start_result;
}

static struct fake_io_step next_step(const struct fake_io_step* steps,
                                     size_t count, size_t* index,
                                     int default_result,
                                     size_t default_bytes)
{
    struct fake_io_step step;
    memset(&step, 0, sizeof(step));
    step.result = default_result;
    step.bytes = default_bytes;
    if (*index < count)
        step = steps[*index];
    ++*index;
    return step;
}

static int fake_connect_finish(void* user, int socket, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    struct fake_io_step step;
    ++fake->finish_calls;
    if (socket != fake->descriptor)
        ++failures;
    step = next_step(fake->finish, fake->finish_count, &fake->finish_index,
                     VP_VITA_TCP_IO_OK, 0u);
    advance_clock(fake, step.advance_ms);
    *native_error = step.native_error;
    return step.result;
}

static int fake_wait(void* user, int socket, uint32_t timeout_ms,
                     int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    struct fake_io_step step;
    ++fake->wait_calls;
    if (socket != fake->descriptor)
        ++failures;
    fake->last_wait_ms = timeout_ms;
    step = next_step(fake->wait, fake->wait_count, &fake->wait_index,
                     VP_VITA_TCP_IO_OK, 0u);
    advance_clock(fake, step.advance_ms);
    *native_error = step.native_error;
    return step.result;
}

static int fake_send(void* user, int socket, const uint8_t* data, size_t size,
                     size_t* bytes_sent, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    struct fake_io_step step;
    ++fake->send_calls;
    if (socket != fake->descriptor)
        ++failures;
    if (size > fake->largest_send_request)
        fake->largest_send_request = size;
    step = next_step(fake->send, fake->send_count, &fake->send_index,
                     VP_VITA_TCP_IO_OK, size);
    if (fake->send_index > fake->send_count)
        step.bytes = size;
    if (step.result == VP_VITA_TCP_IO_OK && step.bytes <= size &&
        step.bytes <= sizeof(fake->output) - fake->output_size) {
        memcpy(fake->output + fake->output_size, data, step.bytes);
        fake->output_size += step.bytes;
    }
    advance_clock(fake, step.advance_ms);
    *bytes_sent = step.bytes;
    *native_error = step.native_error;
    return step.result;
}

static int fake_shutdown(void* user, int socket, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    ++fake->shutdown_calls;
    if (socket != fake->descriptor)
        ++failures;
    *native_error = fake->shutdown_native_error;
    return fake->shutdown_result;
}

static int fake_close(void* user, int socket, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)user;
    struct fake_io_step step;
    ++fake->close_calls;
    if (socket != fake->descriptor)
        ++failures;
    step = next_step(fake->close, fake->close_count, &fake->close_index,
                     VP_VITA_TCP_IO_OK, 0u);
    *native_error = step.native_error;
    return step.result;
}

static void init_fake(struct fake_socket* fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->now_ms = 100u;
    fake->descriptor = 7;
    fake->open_result = VP_VITA_TCP_IO_OK;
    fake->connect_start_result = VP_VITA_TCP_IO_OK;
    fake->shutdown_result = VP_VITA_TCP_IO_OK;
}

static void make_config(struct vp_vita_tcp_sink_config* config,
                        struct fake_socket* fake)
{
    vp_vita_tcp_sink_config_init(config);
    config->endpoint.ipv4[0] = 192u;
    config->endpoint.ipv4[1] = 168u;
    config->endpoint.ipv4[2] = 1u;
    config->endpoint.ipv4[3] = 42u;
    config->endpoint.port = 18195u;
    config->ops.now_ms = fake_now_ms;
    config->ops.open = fake_open;
    config->ops.connect_start = fake_connect_start;
    config->ops.connect_finish = fake_connect_finish;
    config->ops.wait_writable = fake_wait;
    config->ops.send = fake_send;
    config->ops.shutdown_write = fake_shutdown;
    config->ops.close = fake_close;
    config->ops_user = fake;
}

static int init_connected_sink(struct vp_vita_tcp_sink* sink,
                               struct fake_socket* fake,
                               struct vp_vita_tcp_sink_config* config)
{
    memset(sink, 0, sizeof(*sink));
    init_fake(fake);
    make_config(config, fake);
    if (vp_vita_tcp_sink_init(sink, config) != VP_RESULT_OK)
        return 0;
    return vp_vita_tcp_sink_connect(sink) == VP_RESULT_OK;
}

static void test_defaults_and_validation(void)
{
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink sink;
    struct fake_socket fake;

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    CHECK(config.connect_timeout_ms ==
              VP_VITA_TCP_DEFAULT_CONNECT_TIMEOUT_MS &&
              config.send_timeout_ms == VP_VITA_TCP_DEFAULT_SEND_TIMEOUT_MS &&
              config.max_send_chunk_bytes ==
                  VP_VITA_TCP_DEFAULT_SEND_CHUNK_BYTES,
          "config initializer supplies bounded defaults");
    config.endpoint.port = 0u;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "zero port is rejected");
    make_config(&config, &fake);
    config.ops.send = NULL;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "incomplete injected operations are rejected");
    make_config(&config, &fake);
    config.max_send_calls = 0u;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "unbounded send-call configuration is rejected");
}

static void test_sce_net_timeout_conversion(void)
{
    CHECK(vp_vita_tcp_test_sce_net_timeout_us(0u) == 0,
          "zero milliseconds remains a zero-microsecond poll");
    CHECK(vp_vita_tcp_test_sce_net_timeout_us(100u) == 100000,
          "normal millisecond waits convert exactly to microseconds");
    CHECK(vp_vita_tcp_test_sce_net_timeout_us(UINT32_MAX) == INT_MAX,
          "oversized millisecond waits saturate at SceNet's signed maximum");
}

static void test_sce_net_backend_lifecycle(void)
{
    static const uint8_t payload[3] = {1u, 2u, 3u};
    struct vp_vita_tcp_sce_net_backend backend =
        VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
    struct vp_vita_tcp_socket_ops backend_ops;
    struct vp_vita_tcp_socket_ops rejected_ops;
    struct vp_vita_tcp_socket_ops zero_ops;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink sink;
    struct vp_test_sce_net_state* net;

    vp_test_sce_net_reset();
    net = vp_test_sce_net_get_state();
    net->connect_result = -1;
    net->connect_errno = SCE_NET_EINPROGRESS;
    memset(&backend_ops, 0x5a, sizeof(backend_ops));
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &backend_ops) ==
              VP_RESULT_OK &&
              backend.socket == -1 && backend.epoll == -1 &&
              backend.owned_resources == 0u &&
              backend_ops.open != NULL && backend_ops.close != NULL,
          "SceNet backend first init accepts the zero initializer");

    memset(&sink, 0, sizeof(sink));
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = 192u;
    config.endpoint.ipv4[1] = 168u;
    config.endpoint.ipv4[2] = 1u;
    config.endpoint.ipv4[3] = 42u;
    config.endpoint.port = 18195u;
    config.ops = backend_ops;
    config.ops_user = &backend;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_RESULT_OK &&
              backend.socket == net->socket_result &&
              backend.epoll == net->epoll_create_result &&
              backend.owned_resources != 0u &&
              net->last_wait_timeout_us == 100000,
          "production SceNet backend owns its resources and uses microseconds");
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_RESULT_OK &&
              net->send_calls == 1u,
          "production SceNet send path accepts a complete buffer");

    memset(&rejected_ops, 0, sizeof(rejected_ops));
    memset(&zero_ops, 0, sizeof(zero_ops));
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &rejected_ops) ==
                  VP_ERROR_BUSY &&
              memcmp(&rejected_ops, &zero_ops, sizeof(zero_ops)) == 0 &&
              backend.socket == net->socket_result &&
              backend.epoll == net->epoll_create_result,
          "backend reinit rejects live ownership without changing outputs");

    net->epoll_destroy_failures = 1;
    net->socket_close_failures = 1;
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_ERROR_IO &&
              backend.owned_resources != 0u &&
              backend.epoll == net->epoll_create_result &&
              net->socket_close_calls == 0u,
          "failed epoll destruction retains the complete cleanup obligation");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &rejected_ops) ==
              VP_ERROR_BUSY,
          "reinit remains blocked after a failed epoll close");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_ERROR_IO &&
              backend.owned_resources != 0u && backend.epoll == -1 &&
              backend.socket == net->socket_result &&
              net->socket_close_calls == 1u,
          "failed socket close retains the remaining descriptor obligation");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &rejected_ops) ==
              VP_ERROR_BUSY,
          "reinit remains blocked after a failed socket close");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK &&
              backend.owned_resources == 0u && backend.socket == -1 &&
              backend.epoll == -1 && net->socket_close_calls == 2u,
          "later close retries release every retained SceNet resource");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &backend_ops) ==
                  VP_RESULT_OK &&
              backend.owned_resources == 0u && backend.socket == -1 &&
              backend.epoll == -1,
          "backend reinit succeeds after clean resource release");
}

static void test_sce_net_partial_open_cleanup(void)
{
    struct vp_vita_tcp_sce_net_backend backend =
        VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
    struct vp_vita_tcp_socket_ops ops;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink sink;
    struct vp_test_sce_net_state* net;

    vp_test_sce_net_reset();
    net = vp_test_sce_net_get_state();
    net->setsockopt_result = -1;
    net->socket_close_failures = 1;
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &ops) == VP_RESULT_OK,
          "initialize setsockopt-failure backend");
    memset(&sink, 0, sizeof(sink));
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = 127u;
    config.endpoint.ipv4[3] = 1u;
    config.endpoint.port = 18195u;
    config.ops = ops;
    config.ops_user = &backend;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              backend.owned_resources != 0u &&
              backend.socket == net->socket_result &&
              net->socket_close_calls == 1u &&
              net->epoll_destroy_calls == 0u,
          "setsockopt failure retains a socket when immediate cleanup fails");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &ops) == VP_ERROR_BUSY,
          "partial-open cleanup obligation blocks backend reinit");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK &&
              backend.owned_resources == 0u && backend.socket == -1 &&
              net->socket_close_calls == 2u,
          "close retries a retained partial-open socket");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &ops) == VP_RESULT_OK,
          "reinit succeeds after partial socket cleanup");

    vp_test_sce_net_reset();
    net = vp_test_sce_net_get_state();
    net->epoll_control_result = -1;
    memset(&backend, 0, sizeof(backend));
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &ops) == VP_RESULT_OK,
          "initialize epoll-control-failure backend");
    memset(&sink, 0, sizeof(sink));
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = 127u;
    config.endpoint.ipv4[3] = 1u;
    config.endpoint.port = 18195u;
    config.ops = ops;
    config.ops_user = &backend;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              backend.owned_resources == 0u && backend.socket == -1 &&
              backend.epoll == -1 && net->epoll_destroy_calls == 1u &&
              net->socket_close_calls == 1u,
          "epoll-control failure releases both partially acquired resources");
    CHECK(vp_vita_tcp_sce_net_ops_init(&backend, &ops) == VP_RESULT_OK,
          "reinit succeeds after partial socket-and-epoll cleanup");
}

static void test_immediate_connect_and_partial_send(void)
{
    static const uint8_t payload[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink_stats stats;
    struct fake_socket fake;

    CHECK(init_connected_sink(&sink, &fake, &config),
          "immediate connection succeeds");
    sink.max_send_chunk_bytes = 4u;
    fake.send_count = 4u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = 2u;
    fake.send[1].result = VP_VITA_TCP_IO_OK;
    fake.send[1].bytes = 2u;
    fake.send[2].result = VP_VITA_TCP_IO_OK;
    fake.send[2].bytes = 4u;
    fake.send[3].result = VP_VITA_TCP_IO_OK;
    fake.send[3].bytes = 2u;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_RESULT_OK &&
              fake.output_size == sizeof(payload) &&
              memcmp(fake.output, payload, sizeof(payload)) == 0,
          "partial sends consume the complete buffer in order");
    CHECK(vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.bytes_sent == sizeof(payload) && stats.send_calls == 4u &&
              stats.partial_sends == 2u && fake.largest_send_request == 4u,
          "send statistics and configured chunk bound are exact");
    CHECK(fake.observed_endpoint.port == 18195u &&
              fake.observed_endpoint.ipv4[3] == 42u,
          "connection uses a copied endpoint");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK &&
              fake.shutdown_calls == 1u && fake.close_calls == 1u &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.state == VP_VITA_TCP_SINK_CLOSED,
          "clean close half-closes and releases the socket");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK &&
              fake.close_calls == 1u,
          "clean close is idempotent");
}

static void test_async_connect_and_limits(void)
{
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink_stats stats;
    struct fake_socket fake;

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    config.wait_slice_ms = 25u;
    fake.connect_start_result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.wait_count = 3u;
    fake.wait[0].result = VP_VITA_TCP_IO_TIMEOUT;
    fake.wait[0].advance_ms = 10;
    fake.wait[1].result = VP_VITA_TCP_IO_OK;
    fake.wait[1].advance_ms = 10;
    fake.wait[2].result = VP_VITA_TCP_IO_OK;
    fake.wait[2].advance_ms = 10;
    fake.finish_count = 2u;
    fake.finish[0].result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.finish[1].result = VP_VITA_TCP_IO_OK;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_RESULT_OK,
          "nonblocking connect tolerates timeout slices and spurious readiness");
    CHECK(vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.connect_waits == 3u && stats.would_blocks == 2u &&
              fake.last_wait_ms == 25u,
          "connect wait and would-block counters are exact");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK,
          "close async-connect fixture");

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    config.connect_timeout_ms = 20u;
    config.wait_slice_ms = 20u;
    fake.connect_start_result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.wait_count = 1u;
    fake.wait[0].result = VP_VITA_TCP_IO_OK;
    fake.wait[0].advance_ms = 20;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              fake.finish_calls == 0u && fake.close_calls == 1u,
          "connect success after the absolute deadline is rejected and closed");
    CHECK(vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_CONNECT_TIMEOUT &&
              stats.state == VP_VITA_TCP_SINK_FAILED,
          "connect timeout remains observable");

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    config.max_connect_waits = 1u;
    fake.connect_start_result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.wait_count = 1u;
    fake.wait[0].result = VP_VITA_TCP_IO_TIMEOUT;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_CONNECT_LIMIT,
          "connect wait-count cap prevents an infinite stagnant-clock loop");
}

static void test_send_wait_failure_and_deadline(void)
{
    static const uint8_t payload[4] = {1, 2, 3, 4};
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink_stats stats;
    struct fake_socket fake;

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect send-wait fixture");
    fake.send_count = 2u;
    fake.send[0].result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.send[1].result = VP_VITA_TCP_IO_OK;
    fake.send[1].bytes = sizeof(payload);
    fake.wait_count = 1u;
    fake.wait[0].result = VP_VITA_TCP_IO_OK;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_RESULT_OK &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.send_waits == 1u && stats.would_blocks == 1u,
          "would-block send waits and retries");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK,
          "close send-wait fixture");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect partial-error fixture");
    fake.send_count = 2u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = 2u;
    fake.send[1].result = VP_VITA_TCP_IO_ERROR;
    fake.send[1].native_error = -77;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              fake.shutdown_calls == 1u && fake.close_calls == 1u,
          "partial network failure permanently fails and closes the stream");
    CHECK(vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.bytes_sent == 2u &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND &&
              stats.last_native_error == -77 &&
              stats.state == VP_VITA_TCP_SINK_FAILED,
          "partial exposure and native failure are retained in stats");
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
              VP_ERROR_IO,
          "failed sink never reconnects or resumes");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect late-send fixture");
    sink.send_timeout_ms = 10u;
    fake.send_count = 1u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = sizeof(payload);
    fake.send[0].advance_ms = 10;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.bytes_sent == sizeof(payload) &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND_TIMEOUT,
          "late full-send is conservatively reported as an ambiguous timeout");
}

static void test_io_contract_and_send_caps(void)
{
    static const uint8_t payload[4] = {1, 2, 3, 4};
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink_stats stats;
    struct fake_socket fake;

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect zero-progress fixture");
    fake.send_count = 1u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = 0u;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_IO_CONTRACT,
          "zero-progress success cannot spin forever");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect over-report fixture");
    fake.send_count = 1u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = sizeof(payload) + 1u;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_IO_CONTRACT,
          "send byte over-report is rejected");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect send-call-cap fixture");
    sink.max_send_calls = 2u;
    fake.send_count = 2u;
    fake.send[0].result = VP_VITA_TCP_IO_OK;
    fake.send[0].bytes = 1u;
    fake.send[1].result = VP_VITA_TCP_IO_OK;
    fake.send[1].bytes = 1u;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.bytes_sent == 2u &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND_LIMIT,
          "send-call cap bounds repeated tiny progress");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect send-wait-cap fixture");
    sink.max_send_waits = 1u;
    fake.send_count = 2u;
    fake.send[0].result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.send[1].result = VP_VITA_TCP_IO_WOULD_BLOCK;
    fake.wait_count = 1u;
    fake.wait[0].result = VP_VITA_TCP_IO_TIMEOUT;
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
                  VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND_LIMIT,
          "send-wait cap bounds a stagnant clock and unwritable socket");
}

static void test_cleanup_retry_and_clock_regression(void)
{
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink_stats stats;
    struct fake_socket fake;

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect close-retry fixture");
    fake.close_count = 2u;
    fake.close[0].result = VP_VITA_TCP_IO_ERROR;
    fake.close[0].native_error = -88;
    fake.close[1].result = VP_VITA_TCP_IO_OK;
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_CLOSE &&
              stats.close_errors == 1u,
          "close failure retains an explicit cleanup obligation");
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK &&
              fake.close_calls == 2u &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.state == VP_VITA_TCP_SINK_FAILED,
          "explicit close retries cleanup without erasing capture failure");

    CHECK(init_connected_sink(&sink, &fake, &config),
          "connect shutdown-error fixture");
    fake.shutdown_result = VP_VITA_TCP_IO_ERROR;
    fake.shutdown_native_error = -66;
    CHECK(vp_vita_tcp_sink_close(&sink) == VP_ERROR_IO &&
              fake.close_calls == 1u &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_SHUTDOWN &&
              stats.shutdown_errors == 1u,
          "shutdown error still closes the socket but never claims clean EOF");

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    fake.open_result = VP_VITA_TCP_IO_ERROR;
    fake.open_native_error = -55;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              fake.close_calls == 1u &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_OPEN &&
              stats.last_native_error == -55,
          "failed open descriptor is still closed and reported");

    memset(&sink, 0, sizeof(sink));
    init_fake(&fake);
    make_config(&config, &fake);
    fake.open_advance_ms = -1;
    CHECK(vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(&sink) == VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == VP_RESULT_OK &&
              stats.failure == VP_VITA_TCP_FAILURE_CLOCK,
          "monotonic-clock regression fails closed");
}

struct fake_profile_source {
    uint64_t now;
    uint32_t thread;
};

static uint64_t fake_profile_clock(void* user)
{
    return ((struct fake_profile_source*)user)->now;
}

static uint32_t fake_profile_thread(void* user)
{
    return ((struct fake_profile_source*)user)->thread;
}

static void test_stream_writer_integration(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct vp_config core_config;
    struct vp_name_dictionary names;
    struct vp_name_entry entries[2];
    struct vp_name_dictionary_config name_config;
    char name_text[32];
    uint32_t counter_id = 0u;
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_config tcp_config;
    struct fake_socket fake;
    struct vp_stream_writer writer;
    struct vp_stream_writer_config writer_config;
    uint8_t dictionary_buffer[512];
    size_t drained = 0u;
    struct fake_profile_source source = {50u, 3u};

    memset(&core_config, 0, sizeof(core_config));
    core_config.slots = slots;
    core_config.capacity = 8u;
    core_config.clock = fake_profile_clock;
    core_config.clock_user = &source;
    core_config.thread_id = fake_profile_thread;
    core_config.thread_user = &source;
    CHECK(vp_init(&context, &core_config) == VP_RESULT_OK,
          "initialize integrated profiler context");
    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = entries;
    name_config.entry_capacity = 2u;
    name_config.text = name_text;
    name_config.text_capacity = sizeof(name_text);
    CHECK(vp_name_dictionary_init(&names, &name_config) == VP_RESULT_OK &&
              vp_name_dictionary_register(&names, "jobs", &counter_id) ==
                  VP_RESULT_OK &&
              vp_name_dictionary_seal(&names) == VP_RESULT_OK &&
              vp_counter(&context, counter_id, 9) == VP_RESULT_OK,
          "prepare integrated named event");
    CHECK(init_connected_sink(&sink, &fake, &tcp_config),
          "connect stream-writer TCP sink");
    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &context;
    writer_config.names = &names;
    writer_config.write = vp_vita_tcp_sink_write;
    writer_config.write_user = &sink;
    writer_config.dictionary_buffer = dictionary_buffer;
    writer_config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    CHECK(vp_stream_writer_init(&writer, &writer_config) == VP_RESULT_OK &&
              vp_stream_writer_begin(&writer, 40u) == VP_RESULT_OK &&
              vp_stream_writer_drain(&writer, 8u, &drained) == VP_RESULT_OK &&
              drained == 1u && vp_stream_writer_close(&writer) == VP_RESULT_OK &&
              vp_vita_tcp_sink_close(&sink) == VP_RESULT_OK,
          "existing stream writer drains through TCP and closes with EOF");
    CHECK(fake.output_size > VP_WIRE_HEADER_SIZE + VP_WIRE_EVENT_SIZE,
          "integrated TCP capture contains dictionary, header, and event");
}

int main(void)
{
    test_defaults_and_validation();
    test_sce_net_timeout_conversion();
    test_sce_net_backend_lifecycle();
    test_sce_net_partial_open_cleanup();
    test_immediate_connect_and_partial_send();
    test_async_connect_and_limits();
    test_send_wait_failure_and_deadline();
    test_io_contract_and_send_caps();
    test_cleanup_retry_and_clock_regression();
    test_stream_writer_integration();
    if (failures != 0) {
        fprintf(stderr, "%d VitaProfiler TCP test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler Vita TCP sink: all native tests passed");
    return 0;
}
