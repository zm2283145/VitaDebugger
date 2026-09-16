#include "vitadebug_attach_identity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct IdentityFixture {
    VdAttachIdentityProvider provider;
    uint64_t now_ms;
    uint32_t pid;
    VdAttachTargetIdentity identity;
    unsigned int resolve_calls;
    unsigned int reverse_calls;
    unsigned int snapshot_calls;
    unsigned int change_pid_on_resolve;
    unsigned int change_generation_on_snapshot;
    int reverse_mismatch;
    VdAttachInventoryResult forced_result;
} IdentityFixture;

static uint64_t fake_now(void *context) {
    return ((IdentityFixture *)context)->now_ms;
}

static VdAttachInventoryResult fake_resolve(
    void *context, const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    uint32_t *pid, uint64_t deadline_ms) {
    IdentityFixture *fixture = (IdentityFixture *)context;
    (void)deadline_ms;
    ++fixture->resolve_calls;
    if (fixture->forced_result != VD_ATTACH_INVENTORY_FOUND) {
        return fixture->forced_result;
    }
    assert(memcmp(title_id, "UVDBDEMO1", 10u) == 0);
    *pid = fixture->pid;
    if (fixture->change_pid_on_resolve == fixture->resolve_calls) {
        ++*pid;
    }
    return VD_ATTACH_INVENTORY_FOUND;
}

static VdAttachInventoryResult fake_reverse(
    void *context, uint32_t pid,
    char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    uint64_t deadline_ms) {
    IdentityFixture *fixture = (IdentityFixture *)context;
    (void)deadline_ms;
    ++fixture->reverse_calls;
    assert(pid != 0u);
    memcpy(title_id, fixture->reverse_mismatch ? "OTHERAPP1" : "UVDBDEMO1",
           10u);
    return VD_ATTACH_INVENTORY_FOUND;
}

static VdAttachInventoryResult fake_snapshot(
    void *context, uint32_t pid,
    const char expected_title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms) {
    IdentityFixture *fixture = (IdentityFixture *)context;
    (void)deadline_ms;
    ++fixture->snapshot_calls;
    assert(memcmp(expected_title_id, "UVDBDEMO1", 10u) == 0);
    *identity = fixture->identity;
    identity->pid = pid;
    if (fixture->change_generation_on_snapshot == fixture->snapshot_calls) {
        ++identity->target_generation;
    }
    return VD_ATTACH_INVENTORY_FOUND;
}

static void init_fixture(IdentityFixture *fixture) {
    static const char titles[][VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES] = {
        "UVDBDEMO1",
    };
    VdAttachIdentityConfig config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->now_ms = 1000u;
    fixture->pid = 0x10005u;
    memcpy(fixture->identity.title_id, "UVDBDEMO1", 10u);
    fixture->identity.pid = fixture->pid;
    fixture->identity.main_modid = 0x40001234u;
    fixture->identity.main_fingerprint = 0xaabbccddu;
    fixture->identity.target_generation = UINT64_C(0x1122334455667788);
    fixture->forced_result = VD_ATTACH_INVENTORY_FOUND;
    memset(&config, 0, sizeof(config));
    config.callback_context = fixture;
    config.now_ms = fake_now;
    config.resolve_title = fake_resolve;
    config.reverse_title = fake_reverse;
    config.snapshot_trusted = fake_snapshot;
    config.allowed_title_ids = titles;
    config.allowed_title_count = 1u;
    assert(vd_attach_identity_init(&fixture->provider, &config) == 0);
}

static void test_stable_identity(void) {
    IdentityFixture fixture;
    VdAttachTargetIdentity identity;
    init_fixture(&fixture);
    memset(&identity, 0, sizeof(identity));
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_FOUND);
    assert(identity.target_generation == fixture.identity.target_generation);
    assert(fixture.resolve_calls == 2u);
    assert(fixture.reverse_calls == 2u);
    assert(fixture.snapshot_calls == 2u);
}

static void test_fail_closed_boundaries(void) {
    IdentityFixture fixture;
    VdAttachTargetIdentity identity;
    init_fixture(&fixture);
    memset(&identity, 0x55, sizeof(identity));
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "NOTALLOW1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_DENIED);
    assert(identity.pid == 0u);

    fixture.now_ms = 2000u;
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_UNAVAILABLE);
    fixture.now_ms = 1000u;
    fixture.forced_result = VD_ATTACH_INVENTORY_NOT_FOUND;
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_NOT_FOUND);
}

static void test_race_detection(void) {
    IdentityFixture fixture;
    VdAttachTargetIdentity identity;
    init_fixture(&fixture);
    fixture.change_pid_on_resolve = 2u;
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_CHANGED);

    init_fixture(&fixture);
    fixture.change_generation_on_snapshot = 2u;
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_CHANGED);

    init_fixture(&fixture);
    fixture.reverse_mismatch = 1;
    assert(vd_attach_identity_discover_exact(
               &fixture.provider, "UVDBDEMO1", &identity, 2000u) ==
           VD_ATTACH_INVENTORY_CHANGED);
}

static void test_config_is_copied(void) {
    IdentityFixture fixture;
    VdAttachIdentityConfig config;
    char titles[1][VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES] = {"UVDBDEMO1"};
    memset(&fixture, 0, sizeof(fixture));
    fixture.now_ms = 1000u;
    memset(&config, 0, sizeof(config));
    config.callback_context = &fixture;
    config.now_ms = fake_now;
    config.resolve_title = fake_resolve;
    config.reverse_title = fake_reverse;
    config.snapshot_trusted = fake_snapshot;
    config.allowed_title_ids = titles;
    config.allowed_title_count = 1u;
    assert(vd_attach_identity_init(&fixture.provider, &config) == 0);
    memset(titles, 'X', sizeof(titles));
    assert(memcmp(fixture.provider.allowed_title_ids[0], "UVDBDEMO1", 10u) ==
           0);
}

int main(void) {
    test_stable_identity();
    test_fail_closed_boundaries();
    test_race_detection();
    test_config_is_copied();
    puts("attach identity provider host tests: PASS");
    return 0;
}
