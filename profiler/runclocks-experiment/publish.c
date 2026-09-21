#include "publish.h"

#include <stdio.h>

int rc_publish_atomic(
    const char* final_path, rc_publish_write_fn write_contents,
    void* write_context, const struct rc_publish_io* io, void* io_context)
{
    char part_path[160];
    int fd;
    int result;
    int close_result;
    const int part_length = snprintf(
        part_path, sizeof(part_path), "%s.part", final_path);
    if (part_length < 0 || (size_t)part_length >= sizeof(part_path))
        return -1;

    fd = io->open_exclusive(io_context, part_path);
    if (fd < 0)
        return fd;
    result = write_contents(write_context, fd);
    if (result >= 0)
        result = io->sync_fd(io_context, fd);
    close_result = io->close_fd(io_context, fd);
    if (result >= 0 && close_result < 0)
        result = close_result;
    if (result < 0) {
        (void)io->remove_path(io_context, part_path);
        return result;
    }
    result = io->sync_volume(io_context);
    if (result < 0) {
        (void)io->remove_path(io_context, part_path);
        return result;
    }
    result = io->path_exists(io_context, final_path);
    if (result != 0) {
        (void)io->remove_path(io_context, part_path);
        return result < 0 ? result : -1;
    }
    result = io->rename_path(io_context, part_path, final_path);
    if (result < 0) {
        (void)io->remove_path(io_context, part_path);
        return result;
    }
    return 0;
}
