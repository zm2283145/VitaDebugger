#ifndef VITADEVDEPLOY_SFO_H
#define VITADEVDEPLOY_SFO_H

#include "common.h"

int vdev_read_sfo_title_id(const char *path,
                           char title_id[VDEV_TITLE_ID_LEN + 1u]);

#endif
