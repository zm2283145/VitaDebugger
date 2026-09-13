#include <stdio.h>
#include <string.h>

#include "uvdb_thread_control.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int parse_vcont(
    const char* packet,
    const struct uvdb_thread_inventory* inventory,
    struct uvdb_resume_plan* plan)
{
    return uvdb_rsp_parse_vcont(packet, strlen(packet), inventory, plan);
}

int main(void)
{
    struct uvdb_thread_inventory inventory;
    uvdb_thread_inventory_reset(&inventory);
    check(uvdb_thread_inventory_add(&inventory, 0x101) == 1,
          "add stopped thread");
    check(uvdb_thread_inventory_add(&inventory, 0x202) == 1,
          "add worker thread");
    check(uvdb_thread_inventory_add(&inventory, 0x101) == 0,
          "deduplicate inventory");
    check(uvdb_thread_inventory_add(&inventory, 0) < 0,
          "reject nonpositive inventory id");

    int32_t id = 99;
    check(uvdb_rsp_parse_thread_id("0", 1, &id) == 0 && id == 0,
          "parse any-thread id");
    check(uvdb_rsp_parse_thread_id("-1", 2, &id) == 0 && id == -1,
          "parse all-thread id");
    check(uvdb_rsp_parse_thread_id("1a2B", 4, &id) == 0 && id == 0x1a2b,
          "parse exact positive id");
    check(uvdb_rsp_parse_thread_id("", 0, &id) < 0,
          "reject empty thread id");
    check(uvdb_rsp_parse_thread_id("12x", 3, &id) < 0,
          "reject trailing junk");
    check(uvdb_rsp_parse_thread_id("p1.2", 4, &id) < 0,
          "reject unadvertised multiprocess id");
    check(uvdb_rsp_parse_thread_id("80000000", 8, &id) < 0,
          "reject SceUID sign collision");

    uint32_t address = 0;
    check(uvdb_rsp_parse_u32_hex("89abcdef", 8, &address) == 0 &&
          address == UINT32_C(0x89abcdef),
          "parse exact 32-bit resume address");
    check(uvdb_rsp_parse_u32_hex("12x", 3, &address) < 0,
          "reject malformed resume address");
    check(uvdb_rsp_parse_u32_hex("", 0, &address) < 0,
          "reject empty resume address");
    check(uvdb_rsp_parse_u32_hex("-1", 2, &address) < 0,
          "reject signed resume address");
    check(uvdb_rsp_parse_u32_hex("100000000", 9, &address) < 0,
          "reject overflowing resume address");

    struct uvdb_thread_selection selection;
    uvdb_thread_selection_reset(&selection);
    uvdb_thread_selection_note_stop(&selection, 0x101, &inventory);
    check(uvdb_thread_selection_general(&selection, &inventory) == 0x101,
          "Hg0 resolves stopped thread");
    check(uvdb_thread_selection_step(&selection, &inventory) == 0x101,
          "Hc-1 resolves stopped thread for one step");
    check(uvdb_thread_selection_apply(&selection, 'g', "202", 3,
                                      &inventory) == 0 &&
          uvdb_thread_selection_general(&selection, &inventory) == 0x202,
          "Hg selects visible worker");
    check(uvdb_thread_selection_apply(&selection, 'c', "202", 3,
                                      &inventory) == 0 &&
          uvdb_thread_selection_step(&selection, &inventory) == 0x202,
          "Hc selects visible worker");
    check(uvdb_thread_selection_apply(&selection, 'g', "-1", 2,
                                      &inventory) == 0 &&
          uvdb_thread_selection_general(&selection, &inventory) == 0x101,
          "Hg-1 resolves current stopped thread");
    check(uvdb_thread_selection_apply(&selection, 'c', "0", 1,
                                      &inventory) == 0 &&
          uvdb_thread_selection_step(&selection, &inventory) == 0x101,
          "Hc0 resolves current stopped thread");
    check(uvdb_thread_selection_apply(&selection, 'g', "303", 3,
                                      &inventory) < 0,
          "Hg rejects invisible thread");
    check(uvdb_thread_selection_apply(&selection, 'x', "101", 3,
                                      &inventory) < 0,
          "reject unknown H operation");

    check(uvdb_thread_selection_apply(&selection, 'g', "202", 3,
                                      &inventory) == 0 &&
          uvdb_thread_selection_apply(&selection, 'c', "202", 3,
                                      &inventory) == 0,
          "restore specific selectors for reconciliation");
    struct uvdb_thread_inventory changed;
    uvdb_thread_inventory_reset(&changed);
    uvdb_thread_inventory_add(&changed, 0x101);
    uvdb_thread_selection_reconcile(&selection, &changed);
    check(selection.general == 0x202 && selection.resume == 0x202 &&
          uvdb_thread_selection_general(&selection, &changed) < 0 &&
          uvdb_thread_selection_step(&selection, &changed) < 0,
          "stale explicit selectors fail closed");
    selection.stopped = 0x202;
    uvdb_thread_selection_reconcile(&selection, &changed);
    check(selection.stopped == UVDB_RSP_THREAD_ALL,
          "vanished stopped thread is not reported");
    uvdb_thread_selection_note_stop(&selection, 0x101, &changed);

    struct uvdb_resume_plan legacy = {0};
    check(uvdb_thread_selection_plan_legacy(&selection, &changed, 0,
                                             &legacy) < 0,
          "legacy continue rejects stale positive Hc");
    check(uvdb_thread_selection_apply(&selection, 'c', "101", 3,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 0,
                                             &legacy) < 0,
          "legacy continue rejects unsupported selective Hc");
    check(uvdb_thread_selection_plan_legacy(&selection, &changed, 1,
                                             &legacy) < 0,
          "legacy step rejects unsupported selective Hc");
    check(uvdb_thread_selection_apply(&selection, 'c', "0", 1,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 1,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_STEP &&
          legacy.step_thread == 0x101,
          "legacy Hc0 steps the stopped thread in all-stop mode");
    check(uvdb_thread_selection_apply(&selection, 'c', "-1", 2,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 0,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_CONTINUE &&
          legacy.step_thread == UVDB_RSP_THREAD_ALL,
          "legacy Hc-1 continues the process");
    check(uvdb_thread_selection_apply(&selection, 'c', "-1", 2,
                                      &inventory) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &inventory, 1,
                                             &legacy) < 0,
          "legacy Hc-1 rejects multi-thread step");
    uvdb_thread_selection_note_resume(&selection);
    check(selection.stopped == UVDB_RSP_THREAD_ALL,
          "resume clears stopped thread");

    struct uvdb_resume_plan plan = {0};
    check(parse_vcont("vCont;c", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_CONTINUE && plan.step_thread == -1,
          "vCont default continue");
    check(parse_vcont("vCont;s:202;c", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_STEP && plan.step_thread == 0x202,
          "vCont selected step plus default continue");
    check(parse_vcont("vCont;c;s:202", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_CONTINUE,
          "vCont preserves leftmost matching action");
    check(parse_vcont("vCont;s:0;c", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_STEP && plan.step_thread == 0x101,
          "vCont any selects first inventoried stopped thread");
    check(parse_vcont("vCont;s:202;c:-1", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_STEP && plan.step_thread == 0x202,
          "vCont all-thread fallback");
    check(parse_vcont("vCont;s:303;c", &inventory, &plan) < 0,
          "reject stale explicit step selector instead of widening");
    check(parse_vcont("vCont;c:303;c", &inventory, &plan) < 0,
          "reject stale explicit continue selector");
    check(parse_vcont("vCont;s", &inventory, &plan) < 0,
          "reject stepping multiple threads");
    check(parse_vcont("vCont;s:202", &inventory, &plan) < 0,
          "reject leaving unmatched threads stopped");
    check(parse_vcont("vCont;c:101;c:202", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_CONTINUE,
          "accept explicitly covered inventory");
    check(parse_vcont("vCont;s:202;c:101", &inventory, &plan) == 0 &&
          plan.kind == UVDB_RESUME_STEP && plan.step_thread == 0x202,
          "accept explicit selected step and peer continue");
    check(parse_vcont("vCont;C05", &inventory, &plan) < 0,
          "reject unadvertised signal action");
    check(parse_vcont("vCont;x", &inventory, &plan) < 0,
          "reject unknown action");
    check(parse_vcont("vCont;c:12x;c", &inventory, &plan) < 0,
          "reject malformed action thread id");
    check(parse_vcont("vCont;c;", &inventory, &plan) < 0,
          "reject empty trailing action");
    check(parse_vcont("vCont", &inventory, &plan) < 0,
          "reject missing actions");

    struct uvdb_thread_inventory one;
    uvdb_thread_inventory_reset(&one);
    uvdb_thread_inventory_add(&one, 0x101);
    uvdb_thread_selection_reset(&selection);
    uvdb_thread_selection_note_stop(&selection, 0x101, &one);
    check(uvdb_thread_selection_plan_legacy(&selection, &one, 1,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_STEP &&
          legacy.step_thread == 0x101,
          "legacy Hc-1 steps a single-thread process");
    check(parse_vcont("vCont;s", &one, &plan) == 0 &&
          plan.kind == UVDB_RESUME_STEP && plan.step_thread == 0x101,
          "accept default step for single thread");

    struct uvdb_thread_inventory full;
    uvdb_thread_inventory_reset(&full);
    for(int32_t thread = 1;
        thread <= (int32_t)UVDB_THREAD_INVENTORY_CAPACITY; ++thread)
        check(uvdb_thread_inventory_add(&full, thread) == 1,
              "fill bounded inventory");
    check(uvdb_thread_inventory_add(
              &full, (int32_t)UVDB_THREAD_INVENTORY_CAPACITY + 1) < 0,
          "reject inventory overflow");
    full.count = UVDB_THREAD_INVENTORY_CAPACITY + 1;
    check(parse_vcont("vCont;c", &full, &plan) < 0,
          "reject corrupted inventory bound");

    check(!uvdb_stop_cleanup_can_release(0, 1),
          "uncertain stop with UDF patches remains fail-closed");
    check(uvdb_stop_cleanup_can_release(1, 1),
          "coherent stop permits restore-before-release cleanup");
    check(uvdb_stop_cleanup_can_release(0, 0),
          "uncertain stop without patches permits bounded release cleanup");

    if(failures)
        return 1;
    puts("PASS: coherent RSP thread selection and vCont planning");
    return 0;
}
