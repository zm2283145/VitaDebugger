#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr/thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include "vitadebug_attach_auth_listener.h"
#include "vitadebug_attach_auth_listener_vita.h"
#include "vitadebug_attach_auth_store.h"

#ifndef VD_ATTACH_AUTH_LISTENER_PORT
#define VD_ATTACH_AUTH_LISTENER_PORT 18195u
#endif
#ifndef VD_ATTACH_AUTH_BIND_IPV4
#define VD_ATTACH_AUTH_BIND_IPV4 UINT32_C(0x0a0101d9)
#endif
#ifndef VD_ATTACH_AUTH_PEER_NETWORK
#define VD_ATTACH_AUTH_PEER_NETWORK UINT32_C(0x0a010100)
#endif
#ifndef VD_ATTACH_AUTH_PEER_NETMASK
#define VD_ATTACH_AUTH_PEER_NETMASK UINT32_C(0xffffff00)
#endif
#ifndef VD_ATTACH_AUTH_HANDSHAKE_MS
#define VD_ATTACH_AUTH_HANDSHAKE_MS 3000u
#endif

#define VD_AUTH_GATE_STATUS_DIRECTORY "ux0:data/VitaDebugger"
#define VD_AUTH_GATE_STATUS_PATH \
    VD_AUTH_GATE_STATUS_DIRECTORY "/auth-gate.status"
#define VD_AUTH_GATE_PROVISION_PATH \
    VD_AUTH_GATE_STATUS_DIRECTORY "/auth-provision-v1.bin"
#define VD_AUTH_GATE_RECEIPT_PATH \
    VD_AUTH_GATE_STATUS_DIRECTORY "/auth-device-public-v1.bin"
#define VD_AUTH_GATE_RECEIPT_TEMP_PATH \
    VD_AUTH_GATE_STATUS_DIRECTORY "/auth-device-public-v1.bin.new"
#define VD_AUTH_GATE_NET_MEMORY_SIZE (1024u * 1024u)
#define VD_AUTH_GATE_PROVISION_BYTES 72u
#define VD_AUTH_GATE_RECEIPT_BYTES 56u

#if VD_ATTACH_AUTH_HAS_ASSURANCE
extern int vd_attach_auth_hardware_prove_private_storage(
    void *context,
    const char *store_path,
    const char *temporary_path);
extern int vd_attach_auth_hardware_load_floor(
    void *context,
    uint64_t *revision);
extern int vd_attach_auth_hardware_advance_floor(
    void *context,
    uint64_t revision);
#endif

static uint8_t g_network_memory[VD_AUTH_GATE_NET_MEMORY_SIZE]
    __attribute__((aligned(64)));
static VdAttachAuthVitaRuntime g_runtime;
static int g_runtime_prepared;

static uint32_t read_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t read_u64(const uint8_t *data) {
    return ((uint64_t)read_u32(data) << 32) |
           read_u32(data + 4u);
}

static void write_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void write_u64(uint8_t *data, uint64_t value) {
    write_u32(data, (uint32_t)(value >> 32));
    write_u32(data + 4u, (uint32_t)value);
}

static int write_all(SceUID descriptor,
                     const uint8_t *data,
                     size_t size) {
    size_t offset = 0u;
    while (offset < size) {
        SceSSize written = sceIoWrite(
            descriptor, data + offset, (SceSize)(size - offset));
        if (written <= 0) {
            return written < 0 ? (int)written : -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int read_provision_bundle(
    uint8_t bundle[VD_AUTH_GATE_PROVISION_BYTES]) {
    SceIoStat status;
    SceUID descriptor;
    size_t offset = 0u;
    uint8_t extra;
    int result = 0;
    memset(&status, 0, sizeof(status));
    if (sceIoGetstat(VD_AUTH_GATE_PROVISION_PATH, &status) < 0 ||
        !SCE_S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uint64_t)status.st_size != VD_AUTH_GATE_PROVISION_BYTES) {
        return -1;
    }
    descriptor = sceIoOpen(VD_AUTH_GATE_PROVISION_PATH, SCE_O_RDONLY, 0);
    if (descriptor < 0) {
        return descriptor;
    }
    while (offset < VD_AUTH_GATE_PROVISION_BYTES) {
        SceSSize count = sceIoRead(
            descriptor, bundle + offset,
            (SceSize)(VD_AUTH_GATE_PROVISION_BYTES - offset));
        if (count <= 0) {
            result = count < 0 ? (int)count : -1;
            break;
        }
        offset += (size_t)count;
    }
    if (result == 0 && sceIoRead(descriptor, &extra, 1u) != 0) {
        result = -1;
    }
    if (sceIoClose(descriptor) < 0) {
        result = -1;
    }
    if (result < 0 || memcmp(bundle, "VDAP", 4u) != 0 ||
        read_u32(bundle + 4u) != 1u ||
        read_u64(bundle + 8u) == 0u ||
        read_u64(bundle + 16u) != 1u ||
        read_u64(bundle + 24u) == 0u ||
        read_u64(bundle + 32u) == 0u) {
        crypto_wipe(bundle, VD_AUTH_GATE_PROVISION_BYTES);
        return -1;
    }
    return 0;
}

static int write_public_receipt(
    const VdAttachAuthPublicKey *local) {
    uint8_t receipt[VD_AUTH_GATE_RECEIPT_BYTES];
    SceUID descriptor;
    int result;
    memset(receipt, 0, sizeof(receipt));
    memcpy(receipt, "VDAR", 4u);
    write_u32(receipt + 4u, 1u);
    write_u64(receipt + 8u, local->key_id);
    write_u64(receipt + 16u, local->generation);
    memcpy(receipt + 24u, local->public_key, 32u);
    (void)sceIoRemove(VD_AUTH_GATE_RECEIPT_TEMP_PATH);
    descriptor = sceIoOpen(
        VD_AUTH_GATE_RECEIPT_TEMP_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS);
    if (descriptor < 0) {
        return descriptor;
    }
    result = write_all(descriptor, receipt, sizeof(receipt));
    if (result == 0) {
        result = sceIoSyncByFd(descriptor, 0);
    }
    if (sceIoClose(descriptor) < 0) {
        result = -1;
    }
    if (result < 0) {
        (void)sceIoRemove(VD_AUTH_GATE_RECEIPT_TEMP_PATH);
        return result;
    }
    (void)sceIoRemove(VD_AUTH_GATE_RECEIPT_PATH);
    result = sceIoRename(VD_AUTH_GATE_RECEIPT_TEMP_PATH,
                         VD_AUTH_GATE_RECEIPT_PATH);
    if (result >= 0) {
        SceUID directory = sceIoDopen(VD_AUTH_GATE_STATUS_DIRECTORY);
        if (directory < 0) {
            return directory;
        }
        result = sceIoSyncByFd(directory, 0);
        if (sceIoDclose(directory) < 0) {
            result = -1;
        }
    }
    return result;
}

static int provision_from_public_bundle(
    VdAttachAuthKeyStorage *storage) {
    uint8_t bundle[VD_AUTH_GATE_PROVISION_BYTES];
    uint8_t seed[32];
    uint8_t secret[64];
    VdAttachAuthPublicKey local;
    VdAttachAuthPublicKey peer;
    SceIoStat bundle_status;
    uint64_t local_id;
    uint64_t local_generation;
    int result;
    memset(bundle, 0, sizeof(bundle));
    memset(seed, 0, sizeof(seed));
    memset(secret, 0, sizeof(secret));
    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));
    memset(&bundle_status, 0, sizeof(bundle_status));

    result = vd_attach_auth_load_local(storage, &local);
    if (sceIoGetstat(VD_AUTH_GATE_PROVISION_PATH, &bundle_status) < 0) {
        if (result != VD_ATTACH_AUTH_OK) {
            result = VD_ATTACH_AUTH_ERROR_STORAGE;
        }
        goto cleanup;
    }
    if (read_provision_bundle(bundle) < 0) {
        result = VD_ATTACH_AUTH_ERROR_STORAGE;
        goto cleanup;
    }
    local_id = read_u64(bundle + 8u);
    local_generation = read_u64(bundle + 16u);
    if (result != VD_ATTACH_AUTH_OK) {
        if (sceKernelGetRandomNumber(seed, sizeof(seed)) < 0) {
            result = VD_ATTACH_AUTH_ERROR_STORAGE;
            goto cleanup;
        }
        crypto_ed25519_key_pair(secret, local.public_key, seed);
        result = storage->provision_local(
            storage->context, local_id, local_generation, seed,
            local.public_key);
        if (result != VD_ATTACH_AUTH_OK) {
            goto cleanup;
        }
        local.key_id = local_id;
        local.generation = local_generation;
        local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    } else if (local.key_id != local_id ||
               local.generation != local_generation) {
        result = VD_ATTACH_AUTH_ERROR_STORAGE;
        goto cleanup;
    }

    peer.key_id = read_u64(bundle + 24u);
    peer.generation = read_u64(bundle + 32u);
    peer.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    memcpy(peer.public_key, bundle + 40u, 32u);
    result = vd_attach_auth_lookup_peer(
        storage, peer.key_id, peer.generation, &local);
    if (result == VD_ATTACH_AUTH_ERROR_NOT_ALLOWED) {
        result = storage->allow_peer(storage->context, &peer);
    } else if (result == VD_ATTACH_AUTH_ERROR_REVOKED) {
        result = VD_ATTACH_AUTH_ERROR_REVOKED;
    }
    if (result == VD_ATTACH_AUTH_OK) {
        result = vd_attach_auth_load_local(storage, &local);
    }
    if (result == VD_ATTACH_AUTH_OK) {
        result = write_public_receipt(&local) < 0
                     ? VD_ATTACH_AUTH_ERROR_STORAGE
                     : VD_ATTACH_AUTH_OK;
    }
    if (result == VD_ATTACH_AUTH_OK &&
        sceIoRemove(VD_AUTH_GATE_PROVISION_PATH) < 0) {
        result = VD_ATTACH_AUTH_ERROR_STORAGE;
    } else if (result == VD_ATTACH_AUTH_OK) {
        SceUID directory = sceIoDopen(VD_AUTH_GATE_STATUS_DIRECTORY);
        if (directory < 0) {
            result = VD_ATTACH_AUTH_ERROR_STORAGE;
        } else {
            int sync_result = sceIoSyncByFd(directory, 0);
            int close_result = sceIoDclose(directory);
            if (sync_result < 0 || close_result < 0) {
                result = VD_ATTACH_AUTH_ERROR_STORAGE;
            }
        }
    }

cleanup:
    crypto_wipe(bundle, sizeof(bundle));
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(secret, sizeof(secret));
    crypto_wipe(&local, sizeof(local));
    crypto_wipe(&peer, sizeof(peer));
    return result;
}

static void write_status(const char *status) {
    SceUID descriptor;
    size_t size;
    if (status == NULL) {
        return;
    }
    (void)sceIoMkdir(VD_AUTH_GATE_STATUS_DIRECTORY,
                     SCE_S_IRWXU | SCE_S_IRWXS);
    descriptor = sceIoOpen(
        VD_AUTH_GATE_STATUS_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS);
    if (descriptor < 0) {
        return;
    }
    size = strlen(status);
    if (size <= UINT32_MAX) {
        (void)sceIoWrite(descriptor, status, (SceSize)size);
        (void)sceIoSyncByFd(descriptor, 0);
    }
    (void)sceIoClose(descriptor);
}

static int reserve_generation(void *context, uint64_t *generation) {
    return vd_attach_auth_store_reserve_service_generation(
        (VdAttachAuthStore *)context, generation);
}

static void stop_at_exit(void) {
    if (g_runtime_prepared) {
        (void)vd_attach_auth_vita_runtime_stop(&g_runtime);
    }
}

int main(void) {
    VdAttachAuthStore store;
    VdAttachAuthStoreVitaAssurance assurance;
    VdAttachAuthKeyStorage key_storage;
    VdAttachAuthPublicKey local;
    VdAttachAuthListenerConfig config;
    VdAttachAuthListener listener;
    VdAttachAuthSocketOps socket_ops;
    unsigned int previous_buttons = 0u;
    int result;

    memset(&assurance, 0, sizeof(assurance));
#if !VD_ATTACH_AUTH_HAS_ASSURANCE
        write_status(
            "state=blocked\n"
            "reason=hardware-storage-assurance-missing\n"
            "transport=signed-plaintext\n"
            "encryption=none\n");
        return 2;
#else
    assurance.prove_private_storage =
        vd_attach_auth_hardware_prove_private_storage;
    assurance.load_floor = vd_attach_auth_hardware_load_floor;
    assurance.advance_floor =
        vd_attach_auth_hardware_advance_floor;
#endif
    result = vd_attach_auth_store_vita_init(&store, &assurance);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        vd_attach_auth_store_bind(&store, &key_storage) !=
            VD_ATTACH_AUTH_STORE_OK) {
        write_status(
            "state=blocked\n"
            "reason=key-store-unavailable\n"
            "transport=signed-plaintext\n"
            "encryption=none\n");
        return 3;
    }
    result = provision_from_public_bundle(&key_storage);
    if (result != VD_ATTACH_AUTH_OK ||
        vd_attach_auth_load_local(&key_storage, &local) !=
            VD_ATTACH_AUTH_OK) {
        write_status(
            "state=blocked\n"
            "reason=key-store-unprovisioned\n"
            "transport=signed-plaintext\n"
            "encryption=none\n");
        return 3;
    }
    crypto_wipe(&local, sizeof(local));

    result = vd_attach_auth_vita_runtime_prepare(
        &g_runtime, g_network_memory, sizeof(g_network_memory),
        &socket_ops);
    if (result < 0) {
        write_status("state=failed\nreason=network-init\n");
        return 4;
    }
    g_runtime_prepared = 1;
    if (atexit(stop_at_exit) != 0) {
        (void)vd_attach_auth_vita_runtime_stop(&g_runtime);
        g_runtime_prepared = 0;
        write_status("state=failed\nreason=exit-hook\n");
        return 5;
    }

    vd_attach_auth_listener_config_init(&config);
    config.socket_ops = socket_ops;
    config.key_storage = &key_storage;
    config.generation_context = &store;
    config.reserve_service_generation = reserve_generation;
    config.bind_ipv4 = VD_ATTACH_AUTH_BIND_IPV4;
    config.peer_network = VD_ATTACH_AUTH_PEER_NETWORK;
    config.peer_netmask = VD_ATTACH_AUTH_PEER_NETMASK;
    config.port = VD_ATTACH_AUTH_LISTENER_PORT;
    config.handshake_deadline_ms = VD_ATTACH_AUTH_HANDSHAKE_MS;
    result = vd_attach_auth_listener_init(&listener, &config);
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_attach_auth_listener_start(&listener);
    }
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_attach_auth_vita_runtime_start_worker(
            &g_runtime, &listener);
    }
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        (void)vd_attach_auth_listener_shutdown(&listener);
        (void)vd_attach_auth_vita_runtime_stop(&g_runtime);
        g_runtime_prepared = 0;
        write_status("state=failed\nreason=listener-start\n");
        return 6;
    }
    write_status(
        "state=listening\n"
        "protocol=2\n"
        "scope=authentication-only\n"
        "transport=signed-plaintext\n"
        "encryption=none\n");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    for (;;) {
        SceCtrlData pad;
        unsigned int pressed;
        memset(&pad, 0, sizeof(pad));
        (void)sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if ((pressed & SCE_CTRL_CROSS) != 0u) {
            break;
        }
        if (vd_attach_auth_listener_state(&listener) ==
            VD_ATTACH_AUTH_LISTENER_STOPPED) {
            result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
            break;
        }
        sceKernelDelayThread(16000u);
    }

    if (vd_attach_auth_vita_runtime_stop(&g_runtime) < 0) {
        write_status("state=failed\nreason=listener-stop\n");
        return 7;
    }
    g_runtime_prepared = 0;
    write_status(result == VD_ATTACH_AUTH_LISTENER_OK
                     ? "state=stopped\n"
                     : "state=failed\nreason=worker-ended\n");
    return result == VD_ATTACH_AUTH_LISTENER_OK ? 0 : 8;
}
