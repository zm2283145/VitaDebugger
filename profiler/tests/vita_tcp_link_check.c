#include "vitaprofiler_tcp_vita.h"

#include <string.h>

int main(void)
{
    struct vp_vita_tcp_sce_net_backend backend =
        VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
    struct vp_vita_tcp_socket_ops ops;
    struct vp_vita_tcp_sink_config config;
    struct vp_vita_tcp_sink sink;

    memset(&sink, 0, sizeof(sink));
    if (vp_vita_tcp_sce_net_ops_init(&backend, &ops) != VP_RESULT_OK)
        return 1;
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = 127u;
    config.endpoint.ipv4[3] = 1u;
    config.endpoint.port = 18195u;
    config.ops = ops;
    config.ops_user = &backend;
    return vp_vita_tcp_sink_init(&sink, &config) == VP_RESULT_OK ? 0 : 1;
}
