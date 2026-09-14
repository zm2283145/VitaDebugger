#ifndef VITADEVDEPLOY_UI_H
#define VITADEVDEPLOY_UI_H

#include <stdint.h>

int vdev_ui_init(void);
void vdev_ui_finish(void);
void vdev_ui_status(int percent, const char *stage, const char *detail);
void vdev_ui_waiting(void);
void vdev_ui_waiting_tick(void);
void vdev_ui_promotion_progress(int state, uint64_t elapsed_milliseconds,
                                void *context);
void vdev_ui_complete(const char *title_id, const char *detail);
void vdev_ui_error(const char *stage, int code, const char *detail);

#endif
