#include "vitadebug_attach_auth_listener.h"

#include <monocypher-ed25519.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_MAX_CONNECTIONS 40u
#define TEST_IO_BYTES 1024u
#define TEST_HOST_KEY_ID UINT64_C(0x0102030405060708)
#define TEST_SERVER_KEY_ID UINT64_C(0x1112131415161718)
#define TEST_PEER_IPV4 UINT32_C(0x0a01012a)
#define TEST_BIND_IPV4 UINT32_C(0x0a0101d9)

typedef struct FakeStorage {
    uint8_t server_secret[64];
    uint8_t host_secret[64];
    VdAttachAuthPublicKey local;
    VdAttachAuthPublicKey peer;
    int peer_enabled;
    uint64_t persisted_generation;
    uint32_t sign_calls;
} FakeStorage;

typedef struct FakeConnection {
    uint32_t peer_ipv4;
    uint8_t input[TEST_IO_BYTES];
    size_t input_size;
    size_t input_offset;
    uint8_t output[TEST_IO_BYTES];
    size_t output_size;
    size_t max_read;
    size_t max_write;
    uint32_t advance_ms_on_accept;
    int bad_signature;
    int use_stale_proof;
    int disconnect_before_proof;
    int timeout_before_proof;
    int shutdown_during_read;
} FakeConnection;

typedef struct FakeSocket {
    VdAttachAuthListener *listener;
    FakeStorage *storage;
    FakeConnection connections[TEST_MAX_CONNECTIONS];
    size_t connection_count;
    size_t accept_index;
    int active_index;
    int listen_socket;
    uint64_t now_ms;
    uint8_t entropy_byte;
    uint8_t stale_proof[VD_ATTACH_AUTH_FRAME_MAX_BYTES + 4u];
    size_t stale_proof_size;
    int enable_peer_on_accept;
    uint32_t open_calls;
    uint32_t accept_calls;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t shutdown_calls;
    uint32_t close_calls;
    uint32_t active_close_calls;
    uint32_t listen_close_calls;
    uint32_t fail_active_closes;
    uint32_t fail_listen_closes;
    int shutdown_during_open;
} FakeSocket;

static uint32_t test_get_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t test_get_u64(const uint8_t *data) {
    return ((uint64_t)test_get_u32(data) << 32) |
           test_get_u32(data + 4u);
}

static void test_put_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void test_put_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void test_put_u64(uint8_t *data, uint64_t value) {
    test_put_u32(data, (uint32_t)(value >> 32));
    test_put_u32(data + 4u, (uint32_t)value);
}

static void test_header(uint8_t *payload, uint8_t type, uint16_t body_size) {
    memcpy(payload, "VDA2", 4u);
    payload[4] = VD_ATTACH_AUTH_WIRE_VERSION;
    payload[5] = type;
    test_put_u16(payload + 6u, body_size);
}

static int fake_load_local(void *opaque, VdAttachAuthPublicKey *key) {
    *key = ((FakeStorage *)opaque)->local;
    return VD_ATTACH_AUTH_OK;
}

static int fake_lookup_peer(void *opaque,
                            uint64_t key_id,
                            uint64_t generation,
                            VdAttachAuthPublicKey *key) {
    FakeStorage *storage = (FakeStorage *)opaque;
    if (!storage->peer_enabled || storage->peer.key_id != key_id ||
        storage->peer.generation != generation) {
        return VD_ATTACH_AUTH_ERROR_NOT_ALLOWED;
    }
    *key = storage->peer;
    return key->status == VD_ATTACH_AUTH_KEY_REVOKED
               ? VD_ATTACH_AUTH_ERROR_REVOKED
               : VD_ATTACH_AUTH_OK;
}

static int fake_sign_local(
    void *opaque,
    uint64_t key_id,
    uint64_t generation,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms) {
    FakeStorage *storage = (FakeStorage *)opaque;
    if (key_id != storage->local.key_id ||
        generation != storage->local.generation || deadline_ms == 0u) {
        return VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    ++storage->sign_calls;
    crypto_ed25519_sign(signature, storage->server_secret, transcript,
                        transcript_size);
    return VD_ATTACH_AUTH_OK;
}

static VdAttachAuthKeyStorage fake_key_storage(FakeStorage *storage) {
    VdAttachAuthKeyStorage result;
    memset(&result, 0, sizeof(result));
    result.context = storage;
    result.load_local_public = fake_load_local;
    result.lookup_peer = fake_lookup_peer;
    result.sign_local = fake_sign_local;
    return result;
}

static int fake_reserve_generation(void *opaque, uint64_t *generation) {
    FakeStorage *storage = (FakeStorage *)opaque;
    if (storage->persisted_generation == UINT64_MAX) {
        return -1;
    }
    *generation = ++storage->persisted_generation;
    return 0;
}

static uint64_t fake_now_ms(void *opaque) {
    return ((FakeSocket *)opaque)->now_ms;
}

static int fake_entropy(void *opaque, uint8_t *output, size_t size) {
    FakeSocket *fake = (FakeSocket *)opaque;
    size_t index;
    ++fake->entropy_byte;
    if (fake->entropy_byte == 0u) {
        ++fake->entropy_byte;
    }
    for (index = 0u; index < size; ++index) {
        output[index] = (uint8_t)(fake->entropy_byte + index);
        if (output[index] == 0u) {
            output[index] = 1u;
        }
    }
    return 0;
}

static int fake_listen_open(void *opaque,
                            uint32_t bind_ipv4,
                            uint16_t port,
                            int *socket_out) {
    FakeSocket *fake = (FakeSocket *)opaque;
    assert(bind_ipv4 == TEST_BIND_IPV4);
    assert(port == VD_ATTACH_AUTH_LISTENER_DEFAULT_PORT);
    ++fake->open_calls;
    *socket_out = fake->listen_socket;
    if (fake->shutdown_during_open) {
        assert(vd_attach_auth_listener_shutdown(fake->listener) ==
               VD_ATTACH_AUTH_LISTENER_OK);
    }
    return 0;
}

static int fake_accept(void *opaque,
                       int listen_socket,
                       int *socket_out,
                       uint32_t *peer_ipv4,
                       uint64_t deadline_ms) {
    FakeSocket *fake = (FakeSocket *)opaque;
    ++fake->accept_calls;
    assert(listen_socket == fake->listen_socket);
    assert(deadline_ms > fake->now_ms);
    if (fake->accept_index >= fake->connection_count) {
        (void)vd_attach_auth_listener_shutdown(fake->listener);
        return VD_ATTACH_AUTH_LISTENER_ERROR_IO;
    }
    if (fake->enable_peer_on_accept >= 0 &&
        fake->accept_index >= (size_t)fake->enable_peer_on_accept) {
        fake->storage->peer_enabled = 1;
    }
    fake->active_index = (int)fake->accept_index;
    fake->now_ms +=
        fake->connections[fake->accept_index].advance_ms_on_accept;
    *socket_out = 100 + (int)fake->accept_index;
    *peer_ipv4 = fake->connections[fake->accept_index].peer_ipv4;
    ++fake->accept_index;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

static void make_proof(FakeSocket *fake, FakeConnection *connection) {
    const uint8_t *challenge = connection->output + 4u;
    uint8_t transcript[VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES];
    uint8_t *proof;
    size_t domain_size =
        sizeof(VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN) - 1u;
    uint64_t proof_expiry;

    if (connection->use_stale_proof) {
        assert(fake->stale_proof_size != 0u);
        memcpy(connection->input + connection->input_size,
               fake->stale_proof, fake->stale_proof_size);
        connection->input_size += fake->stale_proof_size;
        return;
    }
    assert(connection->input_size + 4u + VD_ATTACH_AUTH_PROOF_BYTES <=
           sizeof(connection->input));
    proof = connection->input + connection->input_size + 4u;
    test_put_u32(connection->input + connection->input_size,
                 VD_ATTACH_AUTH_PROOF_BYTES);
    test_header(proof, 3u,
                VD_ATTACH_AUTH_PROOF_BYTES - 8u);
    memcpy(proof + 8u, challenge + 8u, 72u);
    proof_expiry = test_get_u64(challenge + 64u) + 1000u;
    if (proof_expiry > test_get_u64(challenge + 72u)) {
        proof_expiry = test_get_u64(challenge + 72u);
    }
    test_put_u64(proof + 80u, proof_expiry);
    memcpy(proof + 88u, challenge + 80u, 96u);
    memcpy(transcript, VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN, domain_size);
    memcpy(transcript + domain_size, proof + 8u, 176u);
    crypto_ed25519_sign(proof + 184u, fake->storage->host_secret,
                        transcript, domain_size + 176u);
    if (connection->bad_signature) {
        proof[184u] ^= 1u;
    }
    connection->input_size += 4u + VD_ATTACH_AUTH_PROOF_BYTES;
    if (!connection->bad_signature) {
        memcpy(fake->stale_proof,
               connection->input + connection->input_size -
                   (4u + VD_ATTACH_AUTH_PROOF_BYTES),
               4u + VD_ATTACH_AUTH_PROOF_BYTES);
        fake->stale_proof_size =
            4u + VD_ATTACH_AUTH_PROOF_BYTES;
    }
    crypto_wipe(transcript, sizeof(transcript));
}

static int fake_read(void *opaque,
                     int socket,
                     void *output,
                     size_t size,
                     uint64_t deadline_ms) {
    FakeSocket *fake = (FakeSocket *)opaque;
    FakeConnection *connection;
    size_t available;
    size_t transfer;
    ++fake->read_calls;
    assert(fake->active_index >= 0);
    assert(socket == 100 + fake->active_index);
    connection = &fake->connections[fake->active_index];
    if (connection->shutdown_during_read) {
        connection->shutdown_during_read = 0;
        assert(vd_attach_auth_listener_shutdown(fake->listener) ==
               VD_ATTACH_AUTH_LISTENER_OK);
        return VD_ATTACH_AUTH_LISTENER_ERROR_IO;
    }
    if (connection->input_offset == connection->input_size &&
        connection->output_size >=
            4u + VD_ATTACH_AUTH_CHALLENGE_BYTES) {
        if (connection->timeout_before_proof) {
            fake->now_ms = deadline_ms;
            return VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT;
        }
        if (connection->disconnect_before_proof) {
            return 0;
        }
        make_proof(fake, connection);
    }
    available = connection->input_size - connection->input_offset;
    if (available == 0u) {
        return 0;
    }
    transfer = size < available ? size : available;
    if (connection->max_read != 0u &&
        transfer > connection->max_read) {
        transfer = connection->max_read;
    }
    memcpy(output, connection->input + connection->input_offset,
           transfer);
    connection->input_offset += transfer;
    return (int)transfer;
}

static int fake_write(void *opaque,
                      int socket,
                      const void *data,
                      size_t size,
                      uint64_t deadline_ms) {
    FakeSocket *fake = (FakeSocket *)opaque;
    FakeConnection *connection;
    size_t transfer = size;
    (void)deadline_ms;
    ++fake->write_calls;
    assert(fake->active_index >= 0);
    assert(socket == 100 + fake->active_index);
    connection = &fake->connections[fake->active_index];
    if (connection->max_write != 0u &&
        transfer > connection->max_write) {
        transfer = connection->max_write;
    }
    assert(transfer <= sizeof(connection->output) -
                           connection->output_size);
    memcpy(connection->output + connection->output_size, data,
           transfer);
    connection->output_size += transfer;
    return (int)transfer;
}

static int fake_shutdown(void *opaque, int socket) {
    FakeSocket *fake = (FakeSocket *)opaque;
    (void)socket;
    ++fake->shutdown_calls;
    return 0;
}

static int fake_close(void *opaque, int socket) {
    FakeSocket *fake = (FakeSocket *)opaque;
    ++fake->close_calls;
    if (socket == fake->listen_socket) {
        ++fake->listen_close_calls;
        if (fake->fail_listen_closes != 0u) {
            --fake->fail_listen_closes;
            return -1;
        }
    } else {
        ++fake->active_close_calls;
        if (fake->fail_active_closes != 0u) {
            --fake->fail_active_closes;
            return -1;
        }
    }
    return 0;
}

static void init_storage(FakeStorage *storage) {
    uint8_t host_seed[32];
    uint8_t server_seed[32];
    size_t index;
    memset(storage, 0, sizeof(*storage));
    for (index = 0u; index < 32u; ++index) {
        host_seed[index] = (uint8_t)(index + 1u);
        server_seed[index] = (uint8_t)(index + 33u);
    }
    crypto_ed25519_key_pair(storage->host_secret,
                            storage->peer.public_key, host_seed);
    crypto_ed25519_key_pair(storage->server_secret,
                            storage->local.public_key, server_seed);
    storage->peer.key_id = TEST_HOST_KEY_ID;
    storage->peer.generation = 1u;
    storage->peer.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    storage->local.key_id = TEST_SERVER_KEY_ID;
    storage->local.generation = 1u;
    storage->local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    storage->peer_enabled = 1;
    crypto_wipe(host_seed, sizeof(host_seed));
    crypto_wipe(server_seed, sizeof(server_seed));
}

static void init_socket(FakeSocket *fake, FakeStorage *storage) {
    memset(fake, 0, sizeof(*fake));
    fake->storage = storage;
    fake->active_index = -1;
    fake->listen_socket = 50;
    fake->now_ms = 10000u;
    fake->entropy_byte = 0x40u;
    fake->enable_peer_on_accept = -1;
}

static void add_hello(FakeSocket *fake,
                      uint32_t peer_ipv4,
                      uint8_t nonce_seed) {
    FakeConnection *connection =
        &fake->connections[fake->connection_count++];
    uint8_t *hello = connection->input + 4u;
    size_t index;
    assert(fake->connection_count <= TEST_MAX_CONNECTIONS);
    memset(connection, 0, sizeof(*connection));
    connection->peer_ipv4 = peer_ipv4;
    test_put_u32(connection->input, VD_ATTACH_AUTH_HELLO_BYTES);
    test_header(hello, 1u, VD_ATTACH_AUTH_HELLO_BYTES - 8u);
    test_put_u64(hello + 8u, TEST_HOST_KEY_ID);
    test_put_u64(hello + 16u, 1u);
    for (index = 0u; index < 64u; ++index) {
        hello[24u + index] = (uint8_t)(nonce_seed + index);
        if (hello[24u + index] == 0u) {
            hello[24u + index] = 1u;
        }
    }
    connection->input_size = 4u + VD_ATTACH_AUTH_HELLO_BYTES;
}

static VdAttachAuthListenerConfig make_config(
    FakeSocket *fake,
    FakeStorage *storage,
    VdAttachAuthKeyStorage *keys) {
    VdAttachAuthListenerConfig config;
    vd_attach_auth_listener_config_init(&config);
    *keys = fake_key_storage(storage);
    config.socket_ops.context = fake;
    config.socket_ops.now_ms = fake_now_ms;
    config.socket_ops.entropy = fake_entropy;
    config.socket_ops.listen_open = fake_listen_open;
    config.socket_ops.accept = fake_accept;
    config.socket_ops.read = fake_read;
    config.socket_ops.write = fake_write;
    config.socket_ops.shutdown = fake_shutdown;
    config.socket_ops.close = fake_close;
    config.key_storage = keys;
    config.generation_context = storage;
    config.reserve_service_generation = fake_reserve_generation;
    config.bind_ipv4 = TEST_BIND_IPV4;
    config.peer_network = UINT32_C(0x0a010100);
    config.peer_netmask = UINT32_C(0xffffff00);
    return config;
}

static void run_listener(VdAttachAuthListener *listener,
                         FakeSocket *fake,
                         FakeStorage *storage,
                         VdAttachAuthKeyStorage *keys) {
    VdAttachAuthListenerConfig config =
        make_config(fake, storage, keys);
    fake->listener = listener;
    assert(vd_attach_auth_listener_init(listener, &config) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(vd_attach_auth_listener_start(listener) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(vd_attach_auth_listener_run(listener) ==
           VD_ATTACH_AUTH_LISTENER_OK);
}

static void test_partial_io_and_resource_cleanup(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerStats stats;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x10u);
    fake.connections[0].max_read = 1u;
    fake.connections[0].max_write = 1u;

    run_listener(&listener, &fake, &storage, &keys);
    assert(fake.connections[0].output_size ==
           4u + VD_ATTACH_AUTH_CHALLENGE_BYTES +
               4u + VD_ATTACH_AUTH_RESULT_BYTES);
    assert(test_get_u32(fake.connections[0].output) ==
           VD_ATTACH_AUTH_CHALLENGE_BYTES);
    assert(test_get_u32(
               fake.connections[0].output + 4u +
               VD_ATTACH_AUTH_CHALLENGE_BYTES) ==
           VD_ATTACH_AUTH_RESULT_BYTES);
    assert(vd_attach_auth_listener_get_stats(&listener, &stats) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(stats.authenticated == 1u && stats.accepted == 1u);
    assert(fake.active_close_calls == 1u);
    assert(fake.listen_close_calls == 1u);
    assert(listener.active_socket == -1);
    assert(listener.listen_socket == -1);
}

static void test_timeout_disconnect_malformed_and_peer_policy(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerStats stats;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x20u);
    fake.connections[0].timeout_before_proof = 1;
    add_hello(&fake, TEST_PEER_IPV4, 0x30u);
    fake.connections[1].disconnect_before_proof = 1;
    add_hello(&fake, UINT32_C(0x08080808), 0x40u);
    memset(&fake.connections[fake.connection_count], 0,
           sizeof(fake.connections[fake.connection_count]));
    fake.connections[fake.connection_count].peer_ipv4 =
        TEST_PEER_IPV4;
    test_put_u32(fake.connections[fake.connection_count].input,
                 VD_ATTACH_AUTH_FRAME_MAX_BYTES + 1u);
    fake.connections[fake.connection_count].input_size = 4u;
    ++fake.connection_count;

    run_listener(&listener, &fake, &storage, &keys);
    assert(vd_attach_auth_listener_get_stats(&listener, &stats) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(stats.accepted == 4u);
    assert(stats.io_failures == 2u);
    assert(stats.peer_rejected == 1u);
    assert(stats.malformed == 1u);
    assert(fake.active_close_calls == 4u);
}

static void test_replay_rate_limit_recovery_and_revocation(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerStats stats;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x50u);
    add_hello(&fake, TEST_PEER_IPV4, 0x50u);
    add_hello(&fake, TEST_PEER_IPV4, 0x60u);
    add_hello(&fake, TEST_PEER_IPV4, 0x70u);
    fake.connections[3].advance_ms_on_accept = 250u;

    run_listener(&listener, &fake, &storage, &keys);
    assert(vd_attach_auth_listener_get_stats(&listener, &stats) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(stats.authenticated == 2u);
    assert(stats.replay_rejected == 1u);
    assert(stats.rate_limited == 1u);

    init_socket(&fake, &storage);
    storage.peer.status = VD_ATTACH_AUTH_KEY_REVOKED;
    add_hello(&fake, TEST_PEER_IPV4, 0x71u);
    run_listener(&listener, &fake, &storage, &keys);
    assert(vd_attach_auth_listener_get_stats(&listener, &stats) ==
           VD_ATTACH_AUTH_LISTENER_OK);
    assert(stats.denied == 1u);
}

static void test_bad_proof_backoff_and_recovery(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerConfig config;
    VdAttachAuthListenerStats stats;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x80u);
    fake.connections[0].bad_signature = 1;
    add_hello(&fake, TEST_PEER_IPV4, 0x81u);
    add_hello(&fake, TEST_PEER_IPV4, 0x82u);
    fake.connections[2].advance_ms_on_accept = 250u;
    fake.listener = &listener;
    config = make_config(&fake, &storage, &keys);
    assert(vd_attach_auth_listener_init(&listener, &config) == 0);
    assert(vd_attach_auth_listener_start(&listener) == 0);
    assert(vd_attach_auth_listener_run(&listener) == 0);
    assert(vd_attach_auth_listener_get_stats(&listener, &stats) == 0);
    assert(stats.denied == 1u);
    assert(stats.rate_limited == 1u);
    assert(stats.authenticated == 1u);
}

static void test_failure_table_exhaustion_requires_restart(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerConfig config;
    uint32_t index;
    init_storage(&storage);
    storage.peer_enabled = 0;
    init_socket(&fake, &storage);
    for (index = 0u;
         index < VD_ATTACH_AUTH_LISTENER_MAX_FAILURES + 2u;
         ++index) {
        add_hello(&fake, TEST_PEER_IPV4, (uint8_t)(0xb0u + index));
        test_put_u64(fake.connections[index].input + 4u + 8u,
                     TEST_HOST_KEY_ID + index + 1u);
    }
    run_listener(&listener, &fake, &storage, &keys);
    assert(listener.failure_table_exhausted == 1);
    assert(listener.stats.denied ==
           VD_ATTACH_AUTH_LISTENER_MAX_FAILURES + 1u);
    assert(listener.stats.rate_limited == 1u);

    storage.peer_enabled = 1;
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0xd0u);
    fake.listener = &listener;
    config = make_config(&fake, &storage, &keys);
    listener.config = config;
    assert(vd_attach_auth_listener_start(&listener) == 0);
    assert(vd_attach_auth_listener_run(&listener) == 0);
    assert(listener.failure_table_exhausted == 0);
    assert(listener.stats.authenticated == 1u);
}

static void test_shutdown_cancels_active_socket(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x90u);
    fake.connections[0].shutdown_during_read = 1;
    run_listener(&listener, &fake, &storage, &keys);
    assert(fake.active_close_calls == 1u);
    assert(fake.listen_close_calls == 1u);
    assert(listener.state == VD_ATTACH_AUTH_LISTENER_STOPPED);
    assert(vd_attach_auth_listener_shutdown(&listener) ==
           VD_ATTACH_AUTH_LISTENER_OK);
}

static void test_failed_closes_are_retained_and_retried(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0x95u);
    fake.fail_active_closes = 1u;
    fake.fail_listen_closes = 1u;
    run_listener(&listener, &fake, &storage, &keys);
    assert(fake.active_close_calls == 2u);
    assert(fake.listen_close_calls == 2u);
    assert(listener.stats.close_failures == 2u);
    assert(listener.active_socket == -1);
    assert(listener.listen_socket == -1);
}

static void test_restart_generation_and_stale_proof(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerConfig config;
    uint8_t saved_proof[sizeof(fake.stale_proof)];
    size_t saved_proof_size;
    init_storage(&storage);
    init_socket(&fake, &storage);
    add_hello(&fake, TEST_PEER_IPV4, 0xa0u);
    run_listener(&listener, &fake, &storage, &keys);
    assert(listener.service_generation == 1u);
    saved_proof_size = fake.stale_proof_size;
    memcpy(saved_proof, fake.stale_proof, saved_proof_size);

    init_socket(&fake, &storage);
    memcpy(fake.stale_proof, saved_proof, saved_proof_size);
    fake.stale_proof_size = saved_proof_size;
    add_hello(&fake, TEST_PEER_IPV4, 0xa0u);
    fake.connections[0].use_stale_proof = 1;
    fake.listener = &listener;
    config = make_config(&fake, &storage, &keys);
    assert(vd_attach_auth_listener_init(&listener, &config) == 0);
    assert(vd_attach_auth_listener_start(&listener) == 0);
    assert(listener.service_generation == 2u);
    assert(vd_attach_auth_listener_run(&listener) == 0);
    assert(listener.stats.authenticated == 0u);
    assert(listener.stats.malformed == 1u);
    assert(fake.connections[0].output_size ==
           4u + VD_ATTACH_AUTH_CHALLENGE_BYTES);
}

static void test_configuration_bounds(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerConfig config;
    init_storage(&storage);
    init_socket(&fake, &storage);
    config = make_config(&fake, &storage, &keys);
    config.port = VD_ATTACH_AUTH_LISTENER_MIN_PORT - 1u;
    assert(vd_attach_auth_listener_init(&listener, &config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
    config = make_config(&fake, &storage, &keys);
    config.bind_ipv4 = UINT32_C(0x08080808);
    assert(vd_attach_auth_listener_init(&listener, &config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
    config = make_config(&fake, &storage, &keys);
    config.peer_netmask = UINT32_C(0xff00ff00);
    assert(vd_attach_auth_listener_init(&listener, &config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
}

static void test_network_ownership_modes(void) {
    uint8_t network_memory[VD_ATTACH_AUTH_NETWORK_MEMORY_MIN];
    VdAttachAuthNetworkConfig config;

    memset(&config, 0, sizeof(config));
    config.mode = VD_ATTACH_AUTH_NETWORK_STANDALONE_OWNED;
    config.network_memory = network_memory;
    config.network_memory_size = sizeof(network_memory);
    config.owns_network_module = 1;
    config.owns_network_initialization = 1;
    assert(vd_attach_auth_network_config_validate(&config) ==
           VD_ATTACH_AUTH_LISTENER_OK);

    config.mode = VD_ATTACH_AUTH_NETWORK_SHELL_BORROWED;
    assert(vd_attach_auth_network_config_validate(&config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
    config.network_memory = NULL;
    config.network_memory_size = 0u;
    config.owns_network_module = 0;
    config.owns_network_initialization = 0;
    assert(vd_attach_auth_network_config_validate(&config) ==
           VD_ATTACH_AUTH_LISTENER_OK);

    config.owns_network_initialization = 1;
    assert(vd_attach_auth_network_config_validate(&config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
    config.owns_network_initialization = 0;
    config.owns_network_module = 1;
    assert(vd_attach_auth_network_config_validate(&config) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT);
}

static void test_shutdown_during_start_cannot_reopen_listener(void) {
    FakeStorage storage;
    FakeSocket fake;
    VdAttachAuthKeyStorage keys;
    VdAttachAuthListener listener;
    VdAttachAuthListenerConfig config;
    init_storage(&storage);
    init_socket(&fake, &storage);
    fake.listener = &listener;
    fake.shutdown_during_open = 1;
    config = make_config(&fake, &storage, &keys);
    assert(vd_attach_auth_listener_init(&listener, &config) == 0);
    assert(vd_attach_auth_listener_start(&listener) ==
           VD_ATTACH_AUTH_LISTENER_ERROR_SHUTDOWN);
    assert(vd_attach_auth_listener_state(&listener) ==
           VD_ATTACH_AUTH_LISTENER_STOPPED);
    assert(listener.listen_socket == -1);
    assert(fake.listen_close_calls == 1u);
}

int main(void) {
    test_partial_io_and_resource_cleanup();
    test_timeout_disconnect_malformed_and_peer_policy();
    test_replay_rate_limit_recovery_and_revocation();
    test_bad_proof_backoff_and_recovery();
    test_failure_table_exhaustion_requires_restart();
    test_shutdown_cancels_active_socket();
    test_failed_closes_are_retained_and_retried();
    test_restart_generation_and_stale_proof();
    test_configuration_bounds();
    test_network_ownership_modes();
    test_shutdown_during_start_cannot_reopen_listener();
    puts("attach authentication listener tests passed");
    return 0;
}
