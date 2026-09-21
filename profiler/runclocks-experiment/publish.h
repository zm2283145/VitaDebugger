#ifndef VITAPROFILER_RUNCLOCKS_PUBLISH_H
#define VITAPROFILER_RUNCLOCKS_PUBLISH_H

#include <stddef.h>

struct rc_publish_io {
    int (*open_exclusive)(void* context, const char* path);
    int (*sync_fd)(void* context, int fd);
    int (*close_fd)(void* context, int fd);
    int (*sync_volume)(void* context);
    int (*path_exists)(void* context, const char* path);
    int (*rename_path)(
        void* context, const char* source, const char* destination);
    int (*remove_path)(void* context, const char* path);
};

typedef int (*rc_publish_write_fn)(void* context, int fd);

int rc_publish_atomic(
    const char* final_path, rc_publish_write_fn write_contents,
    void* write_context, const struct rc_publish_io* io, void* io_context);

#endif
