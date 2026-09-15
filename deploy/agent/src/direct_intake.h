#ifndef VITADEVDEPLOY_DIRECT_INTAKE_H
#define VITADEVDEPLOY_DIRECT_INTAKE_H

#include <stddef.h>

typedef struct VdevDirectServer {
    int listener;
    int net_module_loaded;
    int net_initialized;
} VdevDirectServer;

/* Start the optional direct intake listener. A failure does not invalidate the
 * existing filesystem/FTP path; callers may leave the server disabled. */
int vdev_direct_start(VdevDirectServer *server);

/* Poll once for a client. Returns zero after a complete job was committed,
 * one when no client is waiting, and two after a rejected/interrupted client.
 * Negative values describe a local listener failure. */
int vdev_direct_poll(VdevDirectServer *server, const char *challenge_nonce);

void vdev_direct_stop(VdevDirectServer *server);

#endif
