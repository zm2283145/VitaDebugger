#include "vd_endpoint.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GATE_WIDTH 960u
#define GATE_HEIGHT 544u
#define GATE_PITCH 1024u
#define GATE_PIXEL_BYTES 4u
#define GATE_FRAME_BYTES \
    (GATE_PITCH * GATE_HEIGHT * GATE_PIXEL_BYTES)
#define GATE_NET_MEMORY_BYTES (1024u * 1024u)
#define GATE_TRACE_EVENTS 4096u
#define GATE_TRACE_BYTES \
    (VD_INPUT_TRACE_HEADER_SIZE + \
     GATE_TRACE_EVENTS * VD_INPUT_TRACE_EVENT_SIZE)
#define GATE_FRAME_INTERVAL_US UINT64_C(100000)
#define GATE_CLOSE_RETRIES 8u
#define GATE_INVALID_SOCKET (-1)

struct gate_app;

struct gate_transport {
    int listener;
    int client;
    int screen;
    int screen_epoll;
    uint8_t screen_host[4];
    uint8_t control_bind[4];
    struct gate_app* app;
};

struct gate_app {
    struct gate_transport transport;
    struct vd_endpoint_config endpoint_config;
    struct vd_endpoint_receiver receiver;
    struct vd_endpoint_virtual_fs virtual_fs;
    struct vd_companion_service service;
    struct vd_companion_input cooperative_input;
    uint32_t* framebuffers[2];
    struct vd_screen_source sources[2];
    uint8_t* trace_buffer;
    uint32_t display_index;
    uint32_t service_initialized;
    uint32_t screen_started;
    uint32_t client_accepted;
    uint32_t net_module_loaded;
    uint32_t net_initialized;
    uint32_t netctl_initialized;
    uint32_t shutdown_requested;
    int terminal_error;
};

static uint8_t gate_net_memory[GATE_NET_MEMORY_BYTES]
    __attribute__((aligned(64)));

static int gate_would_block(int result)
{
    if ((uint32_t)result == (uint32_t)SCE_NET_ERROR_EAGAIN)
        return 1;
    if (result == -1) {
        int* error = sceNetErrnoLoc();
        return error != NULL &&
               (*error == SCE_NET_EAGAIN ||
                *error == SCE_NET_EWOULDBLOCK ||
                *error == SCE_NET_EINPROGRESS ||
                *error == SCE_NET_EALREADY);
    }
    return 0;
}

static uint64_t gate_now_ms(void* user)
{
    (void)user;
    return sceKernelGetProcessTimeWide() / UINT64_C(1000);
}

static void gate_yield(void* user)
{
    (void)user;
    sceKernelDelayThread(1000u);
}

static int gate_progress(void* user)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    struct gate_app* app;
    SceCtrlData pad;
    int result;

    if (transport == NULL || transport->app == NULL)
        return VD_ENDPOINT_IO_ERROR;
    app = transport->app;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
        (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
            (SCE_CTRL_SELECT | SCE_CTRL_START)) {
        app->shutdown_requested = 1u;
        return VD_ENDPOINT_IO_ERROR;
    }
    if (app->service_initialized == 0u)
        return VD_ENDPOINT_IO_OK;
    result = vd_companion_service_tick(
        &app->service, gate_now_ms(transport));
    return result == VD_COMPANION_OK
               ? VD_ENDPOINT_IO_OK
               : VD_ENDPOINT_IO_ERROR;
}

static int gate_close_socket(int* socket_id)
{
    int result;

    if (socket_id == NULL || *socket_id < 0)
        return 0;
    (void)sceNetSocketAbort(*socket_id, 0);
    (void)sceNetShutdown(*socket_id, SCE_NET_SHUT_RDWR);
    result = sceNetSocketClose(*socket_id);
    if (result < 0)
        return -1;
    *socket_id = GATE_INVALID_SOCKET;
    return 0;
}

static int gate_control_close(void* user)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    int result = 0;

    if (transport == NULL)
        return -1;
    if (gate_close_socket(&transport->client) != 0)
        result = -1;
    if (gate_close_socket(&transport->listener) != 0)
        result = -1;
    return result;
}

static int gate_screen_close(void* user)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    int result = 0;

    if (transport == NULL)
        return -1;
    if (transport->screen_epoll >= 0) {
        if (sceNetEpollDestroy(transport->screen_epoll) < 0)
            result = -1;
        else
            transport->screen_epoll = GATE_INVALID_SOCKET;
    }
    if (gate_close_socket(&transport->screen) != 0)
        result = -1;
    return result;
}

static int gate_control_bind(void* user, uint32_t network_scope,
                             uint16_t port)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    SceNetSockaddrIn address;
    int enabled = 1;
    int result;

    if (transport == NULL || transport->listener >= 0 ||
        port != VD_COMPANION_DEFAULT_CONTROL_PORT)
        return -1;
    transport->listener = sceNetSocket(
        "vitadebug companion control", SCE_NET_AF_INET,
        SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (transport->listener < 0)
        return -1;
    result = sceNetSetsockopt(
        transport->listener, SCE_NET_SOL_SOCKET,
        SCE_NET_SO_REUSEADDR, &enabled,
        (unsigned int)sizeof(enabled));
    if (result >= 0)
        result = sceNetSetsockopt(
            transport->listener, SCE_NET_SOL_SOCKET,
            SCE_NET_SO_NBIO, &enabled,
            (unsigned int)sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(port);
    (void)network_scope;
    address.sin_addr.s_addr = sceNetHtonl(
        ((uint32_t)transport->control_bind[0] << 24) |
        ((uint32_t)transport->control_bind[1] << 16) |
        ((uint32_t)transport->control_bind[2] << 8) |
        (uint32_t)transport->control_bind[3]);
    if (result >= 0)
        result = sceNetBind(
            transport->listener,
            (const SceNetSockaddr*)&address,
            (unsigned int)sizeof(address));
    if (result >= 0)
        result = sceNetListen(transport->listener, 1);
    if (result < 0) {
        (void)gate_close_socket(&transport->listener);
        return -1;
    }
    return 0;
}

static int gate_io_send(void* user, const uint8_t* input, size_t size,
                        size_t* sent)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    int result;

    *sent = 0u;
    if (transport == NULL || transport->client < 0 ||
        size > (size_t)UINT32_MAX)
        return VD_ENDPOINT_IO_ERROR;
    result = sceNetSend(transport->client, input,
                        (unsigned int)size, SCE_NET_MSG_DONTWAIT);
    if (result > 0) {
        *sent = (size_t)result;
        return VD_ENDPOINT_IO_OK;
    }
    return gate_would_block(result)
               ? VD_ENDPOINT_IO_AGAIN
               : VD_ENDPOINT_IO_ERROR;
}

static int gate_io_receive(void* user, uint8_t* output,
                           size_t capacity, size_t* received)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    int result;

    *received = 0u;
    if (transport == NULL || transport->client < 0 ||
        capacity > (size_t)UINT32_MAX)
        return VD_ENDPOINT_IO_ERROR;
    result = sceNetRecv(transport->client, output,
                        (unsigned int)capacity,
                        SCE_NET_MSG_DONTWAIT);
    if (result > 0) {
        *received = (size_t)result;
        return VD_ENDPOINT_IO_OK;
    }
    if (result == 0)
        return VD_ENDPOINT_IO_EOF;
    return gate_would_block(result)
               ? VD_ENDPOINT_IO_AGAIN
               : VD_ENDPOINT_IO_ERROR;
}

static int gate_control_send(void* user, const uint8_t* data,
                             size_t size)
{
    struct vd_endpoint_io io;

    memset(&io, 0, sizeof(io));
    io.user = user;
    io.send = gate_io_send;
    io.now_ms = gate_now_ms;
    io.yield = gate_yield;
    io.progress = gate_progress;
    return vd_endpoint_send_all(
               &io, data, size,
               VD_ENDPOINT_RECEIVE_DEADLINE_MS) == 0
               ? 0
               : -1;
}

static int gate_screen_io_send(void* user, const uint8_t* input,
                               size_t size, size_t* sent)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    int result;

    *sent = 0u;
    if (transport == NULL || transport->screen < 0 ||
        size > (size_t)UINT32_MAX)
        return VD_ENDPOINT_IO_ERROR;
    result = sceNetSend(transport->screen, input,
                        (unsigned int)size, SCE_NET_MSG_DONTWAIT);
    if (result > 0) {
        *sent = (size_t)result;
        return VD_ENDPOINT_IO_OK;
    }
    return gate_would_block(result)
               ? VD_ENDPOINT_IO_AGAIN
               : VD_ENDPOINT_IO_ERROR;
}

static int gate_screen_write(void* user, const uint8_t* data,
                             size_t size)
{
    struct vd_endpoint_io io;

    memset(&io, 0, sizeof(io));
    io.user = user;
    io.send = gate_screen_io_send;
    io.now_ms = gate_now_ms;
    io.yield = gate_yield;
    io.progress = gate_progress;
    return vd_endpoint_send_all(
               &io, data, size,
               VD_ENDPOINT_RECEIVE_DEADLINE_MS) == 0
               ? 0
               : -1;
}

static int gate_screen_connect(void* user, uint32_t network_scope,
                               uint16_t port)
{
    struct gate_transport* transport =
        (struct gate_transport*)user;
    SceNetSockaddrIn address;
    uint64_t deadline;
    uint32_t host;
    int enabled = 1;
    int result;

    (void)network_scope;
    if (transport == NULL || transport->screen >= 0 ||
        port != VD_COMPANION_DEFAULT_SCREEN_PORT)
        return -1;
    transport->screen = sceNetSocket(
        "vitadebug companion screen", SCE_NET_AF_INET,
        SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (transport->screen < 0)
        return -1;
    result = sceNetSetsockopt(
        transport->screen, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
        &enabled, (unsigned int)sizeof(enabled));
    if (result < 0)
        goto fail;
    transport->screen_epoll =
        sceNetEpollCreate("vitadebug companion screen", 0);
    if (transport->screen_epoll < 0)
        goto fail;
    {
        SceNetEpollEvent event;

        memset(&event, 0, sizeof(event));
        event.events =
            SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
        event.data.fd = transport->screen;
        result = sceNetEpollControl(
            transport->screen_epoll, SCE_NET_EPOLL_CTL_ADD,
            transport->screen, &event);
        if (result < 0)
            goto fail;
    }
    host = ((uint32_t)transport->screen_host[0] << 24) |
           ((uint32_t)transport->screen_host[1] << 16) |
           ((uint32_t)transport->screen_host[2] << 8) |
           (uint32_t)transport->screen_host[3];
    memset(&address, 0, sizeof(address));
    address.sin_len = (unsigned char)sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(port);
    address.sin_addr.s_addr = sceNetHtonl(host);
    result = sceNetConnect(
        transport->screen, (const SceNetSockaddr*)&address,
        (unsigned int)sizeof(address));
    if (result == 0)
        goto connected;
    if (!gate_would_block(result))
        goto fail;

    deadline = gate_now_ms(NULL) +
               VD_ENDPOINT_RECEIVE_DEADLINE_MS;
    for (;;) {
        SceNetEpollEvent event;
        int socket_error = 0;
        unsigned int length =
            (unsigned int)sizeof(socket_error);
        const uint64_t now = gate_now_ms(NULL);
        uint64_t remaining;

        if (now >= deadline)
            goto fail;
        remaining = deadline - now;

        memset(&event, 0, sizeof(event));
        result = sceNetEpollWait(
            transport->screen_epoll, &event, 1,
            (int)(remaining * UINT64_C(1000)));
        if (result <= 0)
            goto fail;
        if (event.data.fd != transport->screen ||
            (event.events &
             (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) != 0u)
            goto fail;
        result = sceNetGetsockopt(
            transport->screen, SCE_NET_SOL_SOCKET,
            SCE_NET_SO_ERROR, &socket_error, &length);
        if (result < 0)
            goto fail;
        if (socket_error == 0 ||
            socket_error == SCE_NET_EISCONN ||
            (uint32_t)socket_error ==
                (uint32_t)SCE_NET_ERROR_EISCONN)
            goto connected;
        if (socket_error != SCE_NET_EINPROGRESS &&
            socket_error != SCE_NET_EALREADY &&
            socket_error != SCE_NET_EAGAIN &&
            (uint32_t)socket_error !=
                (uint32_t)SCE_NET_ERROR_EINPROGRESS &&
            (uint32_t)socket_error !=
                (uint32_t)SCE_NET_ERROR_EALREADY &&
            (uint32_t)socket_error !=
                (uint32_t)SCE_NET_ERROR_EAGAIN)
            goto fail;
    }
    goto fail;

connected:
    result = sceNetEpollDestroy(transport->screen_epoll);
    if (result < 0)
        goto fail;
    transport->screen_epoll = GATE_INVALID_SOCKET;
    return 0;

fail:
    (void)gate_screen_close(transport);
    return -1;
}

static int gate_apply_input(
    void* user, const struct vd_companion_input* input)
{
    struct gate_app* app = (struct gate_app*)user;

    if (app == NULL || input == NULL)
        return -1;
    app->cooperative_input = *input;
    return 0;
}

static int gate_read_config(struct vd_endpoint_config* config)
{
    uint8_t data[VD_ENDPOINT_CONFIG_SIZE + 1u];
    SceUID descriptor;
    SceSSize received;
    size_t offset = 0u;
    int result;

    vd_endpoint_config_init(config);
    descriptor = sceIoOpen(
        VD_ENDPOINT_CONFIG_PATH, SCE_O_RDONLY, 0);
    if (descriptor < 0)
        return VD_ENDPOINT_ERROR_CONFIG;
    while (offset < sizeof(data)) {
        received = sceIoRead(
            descriptor, data + offset,
            (SceSize)(sizeof(data) - offset));
        if (received <= 0)
            break;
        offset += (size_t)received;
    }
    result = sceIoClose(descriptor);
    if (result < 0 || offset != VD_ENDPOINT_CONFIG_SIZE)
        return VD_ENDPOINT_ERROR_CONFIG;
    result = vd_endpoint_config_parse(config, data, offset);
    memset(data, 0, sizeof(data));
    return result;
}

static int gate_start_network(struct gate_app* app)
{
    SceNetInitParam init;
    uint64_t deadline;
    int result;

    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0)
        return result;
    app->net_module_loaded = 1u;
    memset(&init, 0, sizeof(init));
    init.memory = gate_net_memory;
    init.size = (int)sizeof(gate_net_memory);
    result = sceNetInit(&init);
    if (result < 0)
        return result;
    app->net_initialized = 1u;
    result = sceNetCtlInit();
    if (result < 0)
        return result;
    app->netctl_initialized = 1u;
    deadline = gate_now_ms(NULL) +
               VD_ENDPOINT_NETWORK_READY_DEADLINE_MS;
    while (gate_now_ms(NULL) < deadline) {
        int state = SCE_NETCTL_STATE_DISCONNECTED;

        result = sceNetCtlInetGetState(&state);
        if (result < 0)
            return result;
        if (state == SCE_NETCTL_STATE_CONNECTED) {
            SceNetCtlInfo info;

            memset(&info, 0, sizeof(info));
            result = sceNetCtlInetGetInfo(
                SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
            if (result < 0)
                return result;
            return vd_endpoint_ipv4_text_matches(
                       app->endpoint_config.bind_ipv4,
                       info.ip_address)
                       ? 0
                       : VD_ENDPOINT_ERROR_CONFIG;
        }
        sceKernelDelayThread(100000u);
    }
    return VD_ENDPOINT_ERROR_DEADLINE;
}

static int gate_random_identity(uint64_t* generation,
                                uint64_t* session_id)
{
    uint64_t values[2];
    int result;

    result = sceKernelGetRandomNumber(values, sizeof(values));
    if (result < 0)
        return result;
    if (values[0] == 0u)
        values[0] = 1u;
    if (values[1] == 0u)
        values[1] = 1u;
    *generation = values[0];
    *session_id = values[1];
    memset(values, 0, sizeof(values));
    return 0;
}

static int gate_initialize_service(struct gate_app* app)
{
    struct vd_companion_screen_config screen;
    struct vd_companion_config config;
    uint64_t generation;
    uint64_t session_id;
    int result;

    result = gate_random_identity(&generation, &session_id);
    if (result < 0)
        return result;
    app->trace_buffer = (uint8_t*)malloc(GATE_TRACE_BYTES);
    if (app->trace_buffer == NULL)
        return -1;
    memcpy(app->transport.screen_host,
           app->endpoint_config.host_ipv4,
           sizeof(app->transport.screen_host));
    memcpy(app->transport.control_bind,
           app->endpoint_config.bind_ipv4,
           sizeof(app->transport.control_bind));
    vd_endpoint_virtual_fs_init(
        &app->virtual_fs, &app->service);
    memset(&screen, 0, sizeof(screen));
    screen.connect = gate_screen_connect;
    screen.close = gate_screen_close;
    screen.transport_user = &app->transport;
    screen.write = gate_screen_write;
    screen.write_user = &app->transport;
    screen.sources = app->sources;
    screen.source_count = 2u;
    memcpy(screen.auth_token,
           app->endpoint_config.screen_secret,
           sizeof(screen.auth_token));
    screen.max_width = GATE_WIDTH;
    screen.max_height = GATE_HEIGHT;
    screen.max_payload_bytes = GATE_FRAME_BYTES;
    screen.min_frame_interval_us = GATE_FRAME_INTERVAL_US;

    vd_companion_config_init(&config);
    config.explicit_consent = VD_COMPANION_EXPLICIT_CONSENT;
    config.lan_consent =
        app->endpoint_config.network_scope ==
                VD_COMPANION_NETWORK_PRIVATE_LAN
            ? VD_COMPANION_LAN_CONSENT
            : 0u;
    config.mutation_consent =
        app->endpoint_config.mutation_consent;
    config.record_consent = app->endpoint_config.record_consent;
    config.playback_consent =
        app->endpoint_config.playback_consent;
    config.network_scope = app->endpoint_config.network_scope;
    config.screen_port = app->endpoint_config.screen_port;
    config.control_port = app->endpoint_config.control_port;
    config.enabled_capabilities =
        app->endpoint_config.enabled_capabilities;
    memcpy(config.secret, app->endpoint_config.control_secret,
           sizeof(config.secret));
    memcpy(config.title_id, "VDSCRN001", sizeof("VDSCRN001"));
    config.process_id = (uint32_t)sceKernelGetProcessId();
    config.process_generation = generation;
    config.session_id = session_id;
    config.debug_root = VD_ENDPOINT_DEBUG_ROOT;
    config.bind = gate_control_bind;
    config.send = gate_control_send;
    config.close = gate_control_close;
    config.transport_user = &app->transport;
    config.now_ms = gate_now_ms;
    config.clock_user = NULL;
    config.apply_input = gate_apply_input;
    config.input_user = app;
    config.filesystem.user = &app->virtual_fs;
    config.filesystem.resolve = vd_endpoint_fs_resolve;
    config.filesystem.list = vd_endpoint_fs_list;
    config.filesystem.read_regular = vd_endpoint_fs_read;
    config.screen = &screen;
    config.trace_buffer = app->trace_buffer;
    config.trace_buffer_capacity = GATE_TRACE_BYTES;
    config.trace_max_events = GATE_TRACE_EVENTS;
    config.trace_max_duration_us = UINT64_C(3600000000);
    config.trace_max_scheduling_drift_us = UINT64_C(100000);
    result = vd_companion_service_init(&app->service, &config);
    memset(config.secret, 0, sizeof(config.secret));
    memset(screen.auth_token, 0, sizeof(screen.auth_token));
    if (result != VD_COMPANION_OK)
        return result;
    app->service_initialized = 1u;
    if ((config.enabled_capabilities &
         VD_COMPANION_CAP_SCREEN) != 0u) {
        result = vd_companion_screen_begin(&app->service);
        if (result != VD_COMPANION_OK)
            return result;
        app->screen_started = 1u;
    }
    return 0;
}

static int gate_accept_once(struct gate_app* app)
{
    int enabled = 1;
    int client;
    int result;

    if (app->client_accepted != 0u)
        return 0;
    client = sceNetAccept(app->transport.listener, NULL, NULL);
    if (client < 0)
        return gate_would_block(client) ? 0 : -1;
    app->transport.client = client;
    app->client_accepted = 1u;
    result = sceNetSetsockopt(
        client, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &enabled,
        (unsigned int)sizeof(enabled));
    if (result < 0)
        return -1;
    vd_endpoint_receiver_init(&app->receiver);
    result = vd_endpoint_receiver_arm(
        &app->receiver, gate_now_ms(&app->transport),
        VD_ENDPOINT_RECEIVE_DEADLINE_MS);
    if (result != 0)
        return result;
    return 0;
}

static int gate_poll_control(struct gate_app* app)
{
    struct vd_endpoint_io io;
    const uint8_t* record;
    size_t record_size;
    int result;

    if (app->client_accepted == 0u)
        return gate_accept_once(app);
    memset(&io, 0, sizeof(io));
    io.user = &app->transport;
    io.receive = gate_io_receive;
    io.now_ms = gate_now_ms;
    result = vd_endpoint_receiver_poll(
        &app->receiver, &io, &record, &record_size);
    if (result == VD_ENDPOINT_POLL_IDLE)
        return 0;
    if (result != VD_ENDPOINT_POLL_RECORD)
        return result;
    result = vd_companion_service_process(
        &app->service, record, record_size);
    vd_endpoint_receiver_init(&app->receiver);
    return result;
}

static void gate_fill_frame(uint32_t* pixels, uint32_t background,
                            const struct gate_app* app)
{
    uint32_t x;
    uint32_t y;
    uint32_t state_color = app->terminal_error == 0
        ? UINT32_C(0xff34c759)
        : UINT32_C(0xffff3b30);
    uint32_t input_color =
        app->cooperative_input.buttons != 0u
            ? UINT32_C(0xffffcc00)
            : UINT32_C(0xff5ac8fa);

    for (y = 0u; y < GATE_HEIGHT; ++y) {
        uint32_t* row = pixels + y * GATE_PITCH;
        for (x = 0u; x < GATE_PITCH; ++x)
            row[x] = x < GATE_WIDTH ? background : 0u;
    }
    for (y = 32u; y < 96u; ++y) {
        for (x = 32u; x < GATE_WIDTH - 32u; ++x)
            pixels[y * GATE_PITCH + x] = state_color;
    }
    for (y = 128u; y < 256u; ++y) {
        for (x = 64u; x < 256u; ++x)
            pixels[y * GATE_PITCH + x] = input_color;
    }
}

static int gate_present(struct gate_app* app, uint64_t now_us)
{
    struct vd_screen_frame frame;
    SceDisplayFrameBuf display;
    uint32_t* pixels = app->framebuffers[app->display_index];
    int result;

    gate_fill_frame(
        pixels, app->display_index == 0u
                    ? UINT32_C(0xff18202b)
                    : UINT32_C(0xff202b38),
        app);
    memset(&display, 0, sizeof(display));
    display.size = sizeof(display);
    display.base = pixels;
    display.pitch = GATE_PITCH;
    display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    display.width = GATE_WIDTH;
    display.height = GATE_HEIGHT;
    result = sceDisplaySetFrameBuf(
        &display, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (result < 0)
        return result;
    if (app->screen_started != 0u) {
        memset(&frame, 0, sizeof(frame));
        frame.pixels = pixels;
        frame.width = GATE_WIDTH;
        frame.height = GATE_HEIGHT;
        frame.stride_bytes = GATE_PITCH * GATE_PIXEL_BYTES;
        frame.pixel_format = VD_SCREEN_PIXEL_RGBA8888;
        frame.timestamp_us = now_us;
        result = vd_companion_submit_displayed_frame(
            &app->service, &frame);
        if (result != VD_COMPANION_OK &&
            result != VD_SCREEN_DROPPED)
            return result;
    }
    app->display_index ^= 1u;
    return 0;
}

static void gate_physical_input(const SceCtrlData* pad,
                                struct vd_companion_input* input)
{
    memset(input, 0, sizeof(*input));
    input->buttons =
        (uint32_t)pad->buttons & VD_INPUT_BUTTON_ALLOWED_MASK;
    input->left_x = (int16_t)((int)pad->lx - 128);
    input->left_y = (int16_t)((int)pad->ly - 128);
    input->right_x = (int16_t)((int)pad->rx - 128);
    input->right_y = (int16_t)((int)pad->ry - 128);
}

static void gate_trace_controls(struct gate_app* app,
                                const SceCtrlData* pad,
                                uint32_t pressed, uint64_t now_us,
                                uint64_t frame_index)
{
    struct vd_companion_status status;
    struct vd_companion_input physical;
    const uint32_t record_combo =
        SCE_CTRL_SELECT | SCE_CTRL_TRIANGLE;
    const uint32_t playback_combo =
        SCE_CTRL_SELECT | SCE_CTRL_SQUARE;

    if (vd_companion_service_get_status(
            &app->service, &status) != VD_COMPANION_OK)
        return;
    if ((app->endpoint_config.enabled_capabilities &
         VD_COMPANION_CAP_INPUT_RECORD) != 0u &&
        (pad->buttons & record_combo) == record_combo &&
        (pressed & SCE_CTRL_TRIANGLE) != 0u) {
        if (status.trace_state == VD_INPUT_TRACE_STATE_RECORDING)
            (void)vd_companion_input_record_end(
                &app->service, VD_INPUT_TRACE_END_COMPLETE);
        else if (status.trace_state == VD_INPUT_TRACE_STATE_IDLE ||
                 status.trace_state == VD_INPUT_TRACE_STATE_READY)
            (void)vd_companion_input_record_begin(
                &app->service,
                app->service.session_id ^ now_us, now_us);
    }
    if ((app->endpoint_config.enabled_capabilities &
         VD_COMPANION_CAP_INPUT_PLAYBACK) != 0u &&
        (pad->buttons & playback_combo) == playback_combo &&
        (pressed & SCE_CTRL_SQUARE) != 0u &&
        status.trace_state == VD_INPUT_TRACE_STATE_READY)
        (void)vd_companion_input_playback_begin(
            &app->service, now_us);

    if (status.trace_state == VD_INPUT_TRACE_STATE_RECORDING) {
        gate_physical_input(pad, &physical);
        (void)vd_companion_input_record_physical(
            &app->service, &physical, now_us, frame_index);
    } else if (status.trace_state ==
               VD_INPUT_TRACE_STATE_PLAYING) {
        (void)vd_companion_input_playback_tick(
            &app->service, now_us);
    }
}

static int gate_allocate_frames(struct gate_app* app)
{
    size_t index;

    for (index = 0u; index < 2u; ++index) {
        app->framebuffers[index] =
            (uint32_t*)memalign(256u, GATE_FRAME_BYTES);
        if (app->framebuffers[index] == NULL)
            return -1;
        app->sources[index].base = app->framebuffers[index];
        app->sources[index].capacity = GATE_FRAME_BYTES;
    }
    return 0;
}

static void gate_cleanup(struct gate_app* app)
{
    unsigned int retry;

    if (app->service_initialized != 0u) {
        for (retry = 0u; retry < GATE_CLOSE_RETRIES; ++retry) {
            if (vd_companion_service_close(&app->service) ==
                VD_COMPANION_OK)
                break;
            sceKernelDelayThread(10000u);
        }
    } else {
        for (retry = 0u; retry < GATE_CLOSE_RETRIES; ++retry) {
            if (gate_screen_close(&app->transport) == 0 &&
                gate_control_close(&app->transport) == 0)
                break;
            sceKernelDelayThread(10000u);
        }
    }

    if (app->transport.client < 0 &&
        app->transport.listener < 0 &&
        app->transport.screen < 0 &&
        app->transport.screen_epoll < 0) {
        if (app->netctl_initialized != 0u) {
            sceNetCtlTerm();
            app->netctl_initialized = 0u;
        }
        if (app->net_initialized != 0u &&
            sceNetTerm() >= 0)
            app->net_initialized = 0u;
        if (app->net_initialized == 0u &&
            app->net_module_loaded != 0u &&
            sceSysmoduleUnloadModule(SCE_SYSMODULE_NET) >= 0)
            app->net_module_loaded = 0u;
    }
    (void)sceDisplaySetFrameBuf(
        NULL, SCE_DISPLAY_SETBUF_NEXTFRAME);
    free(app->framebuffers[0]);
    free(app->framebuffers[1]);
    if (app->trace_buffer != NULL) {
        memset(app->trace_buffer, 0, GATE_TRACE_BYTES);
        free(app->trace_buffer);
    }
    memset(&app->endpoint_config, 0,
           sizeof(app->endpoint_config));
}

static void gate_enter_terminal(struct gate_app* app, int error)
{
    if (app->terminal_error == 0)
        app->terminal_error = error != 0 ? error : -1;
    app->screen_started = 0u;
    if (app->service_initialized != 0u)
        (void)vd_companion_service_disconnect(&app->service);
    else {
        (void)gate_screen_close(&app->transport);
        (void)gate_control_close(&app->transport);
    }
}

static void gate_retry_terminal_cleanup(struct gate_app* app)
{
    if (app->service_initialized != 0u &&
        app->service.state != VD_COMPANION_STATE_CLOSED)
        (void)vd_companion_service_close(&app->service);
    else if (app->service_initialized == 0u) {
        (void)gate_screen_close(&app->transport);
        (void)gate_control_close(&app->transport);
    }
}

int main(void)
{
    struct gate_app app;
    uint32_t previous_buttons = 0u;
    uint64_t frame_index = 0u;
    int running = 1;

    memset(&app, 0, sizeof(app));
    app.transport.listener = GATE_INVALID_SOCKET;
    app.transport.client = GATE_INVALID_SOCKET;
    app.transport.screen = GATE_INVALID_SOCKET;
    app.transport.screen_epoll = GATE_INVALID_SOCKET;
    app.transport.app = &app;
    (void)sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (gate_allocate_frames(&app) != 0) {
        gate_cleanup(&app);
        return 1;
    }
    app.terminal_error =
        gate_read_config(&app.endpoint_config);
    if (app.terminal_error == 0)
        app.terminal_error = gate_start_network(&app);
    if (app.terminal_error == 0)
        app.terminal_error = gate_initialize_service(&app);
    if (app.terminal_error != 0)
        gate_enter_terminal(&app, app.terminal_error);

    while (running != 0) {
        SceCtrlData pad;
        uint32_t pressed = 0u;
        uint64_t now_us = sceKernelGetProcessTimeWide();

        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            pressed = (uint32_t)pad.buttons &
                      ~previous_buttons;
            previous_buttons = (uint32_t)pad.buttons;
        }
        if ((pad.buttons &
             (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
            (SCE_CTRL_SELECT | SCE_CTRL_START))
            running = 0;
        if (app.shutdown_requested != 0u)
            running = 0;

        if (app.terminal_error == 0) {
            const int poll_result = gate_poll_control(&app);
            if (poll_result != 0) {
                gate_enter_terminal(&app, poll_result);
            } else {
                const int tick_result =
                    vd_companion_service_tick(
                        &app.service, now_us / UINT64_C(1000));
                if (tick_result != VD_COMPANION_OK) {
                    gate_enter_terminal(&app, tick_result);
                } else {
                    gate_trace_controls(
                        &app, &pad, pressed, now_us, frame_index);
                }
            }
        } else {
            gate_retry_terminal_cleanup(&app);
        }
        if (gate_present(&app, now_us) < 0)
            gate_enter_terminal(&app, -1);
        ++frame_index;
        sceKernelDelayThread(16000u);
    }
    gate_cleanup(&app);
    sceKernelExitProcess(app.terminal_error == 0 ? 0 : 1);
    return 0;
}
