#include "vitadebug_attach_auth_store.h"

#ifdef __vita__

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdint.h>
#include <string.h>

#define VD_ATTACH_AUTH_STORE_VITA_DIRECTORY \
    "ur0:data/VitaDebugger/private"
#define VD_VITA_ERROR_ENOENT UINT32_C(0x80010002)

static int vd_vita_prove(VdAttachAuthStoreVitaAssurance *assurance) {
    if (assurance == NULL ||
        assurance->prove_private_storage == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    return assurance->prove_private_storage(
               assurance->context, VD_ATTACH_AUTH_STORE_VITA_PATH,
               VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH) ==
                   VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_STORE_OK
               : VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
}

static void *vd_vita_handle(SceUID descriptor) {
    return (void *)(intptr_t)((int64_t)descriptor + 1);
}

static SceUID vd_vita_descriptor(void *handle) {
    return (SceUID)((intptr_t)handle - 1);
}

static int vd_vita_open_read(void *context, const char *path,
                             void **handle) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    SceIoStat status;
    SceUID descriptor;
    int result;

    *handle = NULL;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    memset(&status, 0, sizeof(status));
    result = sceIoGetstat(path, &status);
    if (result < 0) {
        return (uint32_t)result == VD_VITA_ERROR_ENOENT
                   ? VD_ATTACH_AUTH_STORE_IO_NOT_FOUND
                   : VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (!SCE_S_ISREG(status.st_mode) || status.st_size < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    descriptor = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (descriptor < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    *handle = vd_vita_handle(descriptor);
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int vd_vita_create_new(void *context, const char *path,
                              void **handle) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    SceIoStat status;
    SceUID descriptor;

    *handle = NULL;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    descriptor = sceIoOpen(
        path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0600);
    if (descriptor < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    memset(&status, 0, sizeof(status));
    if (sceIoGetstat(path, &status) < 0 ||
        !SCE_S_ISREG(status.st_mode)) {
        (void)sceIoClose(descriptor);
        (void)sceIoRemove(path);
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    *handle = vd_vita_handle(descriptor);
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int vd_vita_read(void *context, void *handle, uint8_t *data,
                        size_t capacity, size_t *read_size) {
    SceSSize result;
    (void)context;
    result = sceIoRead(vd_vita_descriptor(handle), data,
                       (SceSize)capacity);
    if (result < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    *read_size = (size_t)result;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int vd_vita_write(void *context, void *handle,
                         const uint8_t *data, size_t size,
                         size_t *write_size) {
    SceSSize result;
    (void)context;
    result = sceIoWrite(vd_vita_descriptor(handle), data,
                        (SceSize)size);
    if (result < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    *write_size = (size_t)result;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int vd_vita_sync(void *context, void *handle) {
    (void)context;
    return sceIoSyncByFd(vd_vita_descriptor(handle), 0) >= 0
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_IO;
}

static int vd_vita_close(void *context, void *handle) {
    (void)context;
    return sceIoClose(vd_vita_descriptor(handle)) >= 0
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_IO;
}

static int vd_vita_replace(void *context, const char *from,
                           const char *to) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    SceIoStat status;
    int result;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    memset(&status, 0, sizeof(status));
    if (sceIoGetstat(from, &status) < 0 ||
        !SCE_S_ISREG(status.st_mode)) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    result = sceIoGetstat(to, &status);
    if (result >= 0 && !SCE_S_ISREG(status.st_mode)) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (result < 0 &&
        (uint32_t)result != VD_VITA_ERROR_ENOENT) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    return sceIoRename(from, to) >= 0
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_IO;
}

static int vd_vita_remove(void *context, const char *path) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    SceIoStat status;
    int result;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    memset(&status, 0, sizeof(status));
    result = sceIoGetstat(path, &status);
    if (result < 0) {
        return (uint32_t)result == VD_VITA_ERROR_ENOENT
                   ? VD_ATTACH_AUTH_STORE_IO_NOT_FOUND
                   : VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (!SCE_S_ISREG(status.st_mode)) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    return sceIoRemove(path) >= 0
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_IO;
}

static int vd_vita_sync_parent(void *context, const char *path) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    SceUID descriptor;
    int result;
    int close_result;
    (void)path;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    descriptor = sceIoDopen(VD_ATTACH_AUTH_STORE_VITA_DIRECTORY);
    if (descriptor < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    result = sceIoSyncByFd(descriptor, 0);
    close_result = sceIoDclose(descriptor);
    if (result < 0 || close_result < 0 ||
        sceIoSync("ur0:", 0) < 0) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int vd_vita_load_floor(void *context, uint64_t *revision) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK ||
        assurance->load_floor == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    return assurance->load_floor(assurance->context, revision) ==
                   VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
}

static int vd_vita_advance_floor(void *context, uint64_t revision) {
    VdAttachAuthStoreVitaAssurance *assurance =
        (VdAttachAuthStoreVitaAssurance *)context;
    if (vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK ||
        assurance->advance_floor == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    return assurance->advance_floor(assurance->context, revision) ==
                   VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_STORE_IO_OK
               : VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
}

int vd_attach_auth_store_vita_init(
    VdAttachAuthStore *store,
    VdAttachAuthStoreVitaAssurance *assurance) {
    VdAttachAuthStoreFileOps files;
    VdAttachAuthStoreRollbackOps rollback;
    if (store == NULL || assurance == NULL ||
        assurance->prove_private_storage == NULL ||
        assurance->load_floor == NULL ||
        assurance->advance_floor == NULL ||
        vd_vita_prove(assurance) != VD_ATTACH_AUTH_STORE_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    memset(&files, 0, sizeof(files));
    files.context = assurance;
    files.open_read_regular_no_follow = vd_vita_open_read;
    files.create_new_regular_no_follow = vd_vita_create_new;
    files.read = vd_vita_read;
    files.write = vd_vita_write;
    files.sync = vd_vita_sync;
    files.close = vd_vita_close;
    files.replace_atomic = vd_vita_replace;
    files.remove_regular_no_follow = vd_vita_remove;
    files.sync_parent = vd_vita_sync_parent;
    memset(&rollback, 0, sizeof(rollback));
    rollback.context = assurance;
    rollback.load_floor = vd_vita_load_floor;
    rollback.advance_floor = vd_vita_advance_floor;
    return vd_attach_auth_store_init(store, &files, &rollback);
}

#else

int vd_attach_auth_store_vita_init(
    VdAttachAuthStore *store,
    VdAttachAuthStoreVitaAssurance *assurance) {
    (void)store;
    (void)assurance;
    return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
}

#endif
