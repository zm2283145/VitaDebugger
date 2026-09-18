#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stddef.h>

#define VD_AUTH_GATE_STATUS_DIRECTORY "ux0:data/VitaDebugger"
#define VD_AUTH_GATE_STATUS_PATH \
    VD_AUTH_GATE_STATUS_DIRECTORY "/auth-gate.status"

#if !defined(VD_ATTACH_AUTH_HARD_BLOCK) || VD_ATTACH_AUTH_HARD_BLOCK != 1
#error "The retail 3.65 artifact must remain sentinel-only"
#endif

static int write_all(SceUID descriptor,
                     const char *data,
                     size_t size) {
    size_t offset = 0u;
    while (offset < size) {
        SceSSize written = sceIoWrite(
            descriptor, data + offset, (SceSize)(size - offset));
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

int main(void) {
    static const char status[] =
        "state=blocked\n"
        "reason=retail-365-secure-storage-hard-block\n"
        "scope=sentinel-only\n"
        "network=not-started\n"
        "transport=signed-plaintext\n"
        "encryption=none\n";
    SceUID descriptor;
    int result;

    (void)sceIoMkdir(VD_AUTH_GATE_STATUS_DIRECTORY,
                     SCE_S_IRWXU | SCE_S_IRWXS);
    descriptor = sceIoOpen(
        VD_AUTH_GATE_STATUS_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS);
    if (descriptor < 0) {
        return 2;
    }
    result = write_all(descriptor, status, sizeof(status) - 1u);
    if (result == 0 && sceIoSyncByFd(descriptor, 0) < 0) {
        result = -1;
    }
    if (sceIoClose(descriptor) < 0) {
        result = -1;
    }
    return result == 0 ? 2 : 3;
}
