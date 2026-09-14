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

static void check_step_target(
    int result,
    const struct uvdb_step_target* target,
    uint32_t address,
    unsigned int breakpoint_size,
    const char* name)
{
    check(result == 1 && target->address == address &&
          target->breakpoint_size == breakpoint_size, name);
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
    check(uvdb_thread_selection_plan_legacy(&selection, &changed, 1,
                                             &legacy) < 0,
          "legacy step rejects stale positive Hc");
    check(uvdb_thread_selection_apply(&selection, 'c', "101", 3,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 0,
                                             &legacy) < 0,
          "legacy continue rejects unsupported selective Hc");
    check(uvdb_thread_selection_plan_legacy(&selection, &changed, 1,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_STEP &&
          legacy.step_thread == 0x101 &&
          legacy.scope == UVDB_RESUME_SCOPE_STOPPED_THREAD,
          "legacy positive Hc isolates the stopped thread for one step");
    check(uvdb_thread_selection_apply(&selection, 'c', "202", 3,
                                      &inventory) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &inventory, 1,
                                             &legacy) < 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &inventory, 0,
                                             &legacy) < 0,
          "legacy Hc cannot isolate a different visible thread");
    check(uvdb_thread_selection_apply(&selection, 'c', "0", 1,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 1,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_STEP &&
          legacy.step_thread == 0x101 &&
          legacy.scope == UVDB_RESUME_SCOPE_PROCESS,
          "legacy Hc0 steps the stopped thread in all-stop mode");
    check(uvdb_thread_selection_apply(&selection, 'c', "-1", 2,
                                      &changed) == 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &changed, 0,
                                             &legacy) == 0 &&
          legacy.kind == UVDB_RESUME_CONTINUE &&
          legacy.step_thread == UVDB_RSP_THREAD_ALL &&
          legacy.scope == UVDB_RESUME_SCOPE_PROCESS,
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

    struct uvdb_resume_plan isolated = {
        .kind = UVDB_RESUME_STEP,
        .step_thread = 0x101,
        .scope = UVDB_RESUME_SCOPE_STOPPED_THREAD,
    };
    check(uvdb_resume_plan_validate(
              &isolated, 0x101, 1, 1, 0, 0) == 0,
          "selected stopped-thread step requires a healthy stop session");
    check(uvdb_resume_plan_validate(
              &isolated, 0x202, 1, 1, 0, 0) < 0 &&
          uvdb_resume_plan_validate(
              &isolated, 0x101, 0, 1, 0, 0) < 0 &&
          uvdb_resume_plan_validate(
              &isolated, 0x101, 1, 0, 0, 0) < 0 &&
          uvdb_resume_plan_validate(
              &isolated, 0x101, 1, 1, 1, 0) < 0 &&
          uvdb_resume_plan_validate(
              &isolated, 0x101, 1, 1, 0, 1) < 0,
          "selected step rejects wrong owner, running target, missing or failed stop, and PC override");
    isolated.kind = UVDB_RESUME_CONTINUE;
    check(uvdb_resume_plan_validate(
              &isolated, 0x101, 1, 1, 0, 0) < 0,
          "selected-thread continue stays fail-closed");
    isolated.kind = UVDB_RESUME_STEP;
    isolated.scope = UVDB_RESUME_SCOPE_PROCESS;
    check(uvdb_resume_plan_validate(
              &isolated, -1, 0, 0, 1, 1) == 0,
          "process-wide resume does not claim selected-thread isolation");

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

    struct uvdb_thread_inventory corrupt = { .ids = {0x101, 0}, .count = 2 };
    check(!uvdb_thread_inventory_contains(&corrupt, 0x101),
          "reject inventory containing a nonpositive id");
    check(uvdb_thread_selection_apply(&selection, 'c', "0", 1,
                                      &corrupt) < 0 &&
          parse_vcont("vCont;c", &corrupt, &plan) < 0,
          "selectors reject malformed inventory");
    corrupt.ids[1] = 0x101;
    check(!uvdb_thread_inventory_contains(&corrupt, 0x101) &&
          uvdb_thread_selection_plan_legacy(&selection, &corrupt, 0,
                                             &legacy) < 0,
          "reject duplicate inventory ids");
    struct uvdb_thread_inventory empty;
    uvdb_thread_inventory_reset(&empty);
    check(uvdb_thread_selection_apply(&selection, 'g', "0", 1,
                                      &empty) < 0,
          "reject selection without a visible thread");
    selection.general = -2;
    selection.resume = -2;
    selection.stopped = 0x101;
    check(uvdb_thread_selection_general(&selection, &one) < 0 &&
          uvdb_thread_selection_step(&selection, &one) < 0 &&
          uvdb_thread_selection_plan_legacy(&selection, &one, 0,
                                             &legacy) < 0,
          "invalid negative selectors fail closed");

    check(uvdb_arm_condition_passed(0, UINT32_C(1) << 30),
          "A32 EQ condition passes with Z set");
    check(!uvdb_arm_condition_passed(1, UINT32_C(1) << 30),
          "A32 NE condition fails with Z set");
    check(uvdb_arm_condition_passed(8, UINT32_C(1) << 29),
          "A32 HI condition passes with C set and Z clear");
    check(uvdb_arm_condition_passed(13, UINT32_C(1) << 30),
          "A32 LE condition passes with Z set");
    check(uvdb_arm_condition_passed(14, 0),
          "A32 AL condition always passes");
    check(!uvdb_arm_condition_passed(15, UINT32_MAX) &&
          !uvdb_arm_condition_passed(16, UINT32_MAX),
          "reserved or out-of-range A32 conditions fail closed");

    uint32_t registers[16] = {0};
    struct uvdb_step_target target = {0};
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xea000002), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1010), 4,
                      "plan taken A32 B immediate");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0x1a000002), UINT32_C(0x1000),
                          UINT32_C(1) << 30, registers, &target),
                      &target, UINT32_C(0x1004), 4,
                      "plan untaken conditional A32 B fallthrough");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0x1a000002), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1010), 4,
                      "plan taken conditional A32 B target");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xeafffffc), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x0ff8), 4,
                      "sign extend backward A32 branch");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xeafffffe), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "reject unimplementable A32 branch-to-self step");

    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xfa000000), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1008), 2,
                      "plan A32 BLX immediate H zero as Thumb");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xfb000000), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x100a), 2,
                      "plan A32 BLX immediate H one as Thumb");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xfbffffff), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1006), 2,
                      "sign extend backward A32 BLX immediate");

    registers[0] = UINT32_C(0x2001);
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe12fff10), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x2000), 2,
                      "A32 BX register exchanges to Thumb");
    registers[0] = UINT32_C(0x2002);
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe12fff30), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "A32 BLX register rejects invalid ARM halfword alignment");
    registers[0] = UINT32_C(0x2004);
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe12fff30), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x2004), 4,
                      "A32 BLX register accepts word-aligned ARM target");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0x112fff10), UINT32_C(0x1000),
                          UINT32_C(1) << 30, registers, &target),
                      &target, UINT32_C(0x1004), 4,
                      "untaken conditional A32 BX falls through");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe12fff1f), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1008), 4,
                      "A32 BX reads R15 as architectural PC");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe12fff3f), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "reject unpredictable A32 BLX PC");

    registers[0] = UINT32_C(0x3001);
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe1a0f000), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x3000), 2,
                      "A32 MOV PC register interworks to Thumb");
    registers[0] = UINT32_C(0x3002);
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a0f000), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "A32 MOV PC rejects invalid ARM halfword alignment");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0x01a0f000), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1004), 4,
                      "untaken conditional A32 MOV PC falls through");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe1a0f00f), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1008), 4,
                      "A32 MOV PC reads R15 as architectural PC");

    registers[0] = UINT32_C(0x2000);
    registers[1] = UINT32_C(3);
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe280f004), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x2004), 4,
                      "plan A32 ADD PC immediate");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe080f101), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x200c), 4,
                      "plan A32 ADD PC shifted register");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe2a0f000), UINT32_C(0x1000),
                          UINT32_C(1) << 29, registers, &target),
                      &target, UINT32_C(0x2000), 2,
                      "plan A32 ADC PC with carry and interworking");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0xe2c0f000), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1ffe), 2,
                      "plan A32 SBC PC with inverted borrow");
    check_step_target(uvdb_arm_plan_direct_step(
                          UINT32_C(0x0280f004), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1004), 4,
                      "condition-failed A32 ALU PC write falls through");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a0f110), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "reject unpredictable A32 PC write with register shift");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe290f004), UINT32_C(0x1000), 0,
              registers, &target) == 0 &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xe290f004)),
          "leave A32 exception-return ALU write at fail-closed gate");

    uint32_t load_address = 0;
    registers[0] = UINT32_C(0x4000);
    registers[1] = UINT32_C(3);
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe590f004), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x4004),
          "plan A32 immediate LDR PC address");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe510f004), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x3ffc),
          "plan A32 negative immediate LDR PC address");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe490f004), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x4000),
          "plan A32 post-index LDR PC from original base");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe790f101), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x400c),
          "plan A32 register-offset LDR PC address");
    registers[1] = UINT32_C(0x20);
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe710f1c1), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x3ffc),
          "plan A32 negative shifted LDR PC address");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe59ff004), UINT32_C(0x1000), 0,
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x100c),
          "plan A32 literal LDR PC address");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0x0590f004), UINT32_C(0x1000), 0,
              registers, &load_address) == 0,
          "condition-failed A32 LDR PC defers to fallthrough");
    check(uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe5d0f000), UINT32_C(0x1000), 0,
              registers, &load_address) < 0 &&
          uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe79ff001), UINT32_C(0x1000), 0,
              registers, &load_address) < 0 &&
          uvdb_arm_plan_load_pc_address(
              UINT32_C(0xe4b0f004), UINT32_C(0x1000), 0,
              registers, &load_address) < 0,
          "reject unsafe A32 byte, PC-base register, and LDRT PC forms");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a00000), UINT32_C(0x1000), 0,
              registers, &target) == 0,
          "leave ordinary A32 instruction to sequential stepping");
    check(uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a00000), UINT32_C(0x1002), 0,
              registers, &target) < 0 &&
          uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a00000), UINT32_C(0x1000), 0,
              NULL, &target) < 0 &&
          uvdb_arm_plan_direct_step(
              UINT32_C(0xe1a00000), UINT32_C(0x1000), 0,
              registers, NULL) < 0,
          "reject invalid A32 step planner arguments");

    check(uvdb_arm_instruction_may_write_pc(UINT32_C(0xe080f001)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xe1a0f081)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xe1a0f110)),
          "identify unsupported A32 ALU writes to PC");
    check(uvdb_arm_instruction_may_write_pc(UINT32_C(0xe790f001)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xe12fff20)),
          "identify register-offset LDR PC and BXJ");
    check(uvdb_arm_instruction_may_write_pc(UINT32_C(0xe8bd8000)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xea000000)),
          "identify decoded A32 PC-writing classes");
    check(!uvdb_arm_instruction_may_write_pc(UINT32_C(0xe0800001)) &&
          !uvdb_arm_instruction_may_write_pc(UINT32_C(0xe5900000)) &&
          !uvdb_arm_instruction_may_write_pc(UINT32_C(0xf5d0f000)),
          "do not classify ordinary ALU/load or PLD as PC writes");
    check(uvdb_arm_instruction_may_write_pc(UINT32_C(0xf8b00a00)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xf9110a00)) &&
          uvdb_arm_instruction_may_write_pc(UINT32_C(0xe160006e)),
          "classify privileged A32 return forms for fail-closed stepping");

    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xd102), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1008), 2,
                      "plan taken Thumb conditional branch");
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xd102), UINT32_C(0x1000),
                          UINT32_C(1) << 30, registers, &target),
                      &target, UINT32_C(0x1002), 2,
                      "plan untaken Thumb conditional branch");
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xd1fe), UINT32_C(0x1000),
                          UINT32_C(1) << 30, registers, &target),
                      &target, UINT32_C(0x1002), 2,
                      "inactive Thumb self-branch does not patch current PC");
    check(uvdb_thumb16_plan_direct_step(
              UINT16_C(0xd1fe), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "reject unimplementable taken Thumb self-branch step");
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xe7fc), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x0ffc), 2,
                      "sign extend backward Thumb unconditional branch");

    registers[0] = UINT32_C(4);
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0x4487), UINT32_C(0x1002), 0,
                          registers, &target),
                      &target, UINT32_C(0x1008), 2,
                      "plan Thumb high-register ADD PC");
    registers[1] = UINT32_C(0x3001);
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0x468f), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x3000), 2,
                      "plan Thumb high-register MOV PC");
    registers[2] = UINT32_C(0x3001);
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0x4710), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x3000), 2,
                      "plan Thumb BX register interworking");
    check(uvdb_thumb16_plan_direct_step(
              UINT16_C(0x4701), UINT32_C(0x1000), 0,
              registers, &target) < 0 &&
          uvdb_thumb16_plan_direct_step(
              UINT16_C(0x47f8), UINT32_C(0x1000), 0,
              registers, &target) < 0,
          "reject reserved Thumb BX family bits and unpredictable BLX PC");
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0x4778), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1004), 4,
                      "plan deprecated Thumb BX PC to ARM");

    registers[3] = 0;
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xb11b), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x100a), 2,
                      "plan taken Thumb CBZ");
    registers[3] = 1;
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xb11b), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x1002), 2,
                      "plan untaken Thumb CBZ");
    check_step_target(uvdb_thumb16_plan_direct_step(
                          UINT16_C(0xb91b), UINT32_C(0x1000), 0,
                          registers, &target),
                      &target, UINT32_C(0x100a), 2,
                      "plan taken Thumb CBNZ");
    check(uvdb_thumb16_plan_direct_step(
              UINT16_C(0x2000), UINT32_C(0x1000), 0,
              registers, &target) == 0,
          "leave ordinary Thumb instruction to sequential stepping");
    check(uvdb_thumb16_plan_direct_step(
              UINT16_C(0x2000), UINT32_C(0x1001), 0,
              registers, &target) < 0,
          "reject misaligned Thumb step planner PC");
    check(uvdb_thumb16_instruction_may_write_pc(UINT16_C(0x4487)) &&
          uvdb_thumb16_instruction_may_write_pc(UINT16_C(0x4687)) &&
          uvdb_thumb16_instruction_may_write_pc(UINT16_C(0x4700)) &&
          uvdb_thumb16_instruction_may_write_pc(UINT16_C(0xbd00)),
          "identify Thumb16 ADD/MOV/BX/POP writes to PC");
    check(!uvdb_thumb16_instruction_may_write_pc(UINT16_C(0x4400)) &&
          !uvdb_thumb16_instruction_may_write_pc(UINT16_C(0x4600)) &&
          !uvdb_thumb16_instruction_may_write_pc(UINT16_C(0xde00)) &&
          !uvdb_thumb16_instruction_may_write_pc(UINT16_C(0xdf00)),
          "leave ordinary Thumb16 and exception instructions unclassified");

    registers[0] = UINT32_C(0x4000);
    registers[1] = UINT32_C(3);
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf8d0), UINT16_C(0xf004), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x4004),
          "plan Thumb-2 positive imm12 LDR PC address");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xfc04), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x3ffc),
          "plan Thumb-2 negative imm8 LDR PC address");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xff04), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x4004),
          "plan Thumb-2 pre-index writeback LDR PC address");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xfb04), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x4000),
          "plan Thumb-2 post-index LDR PC from original base");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xf021), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x400c),
          "plan Thumb-2 register-offset LDR PC address");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf8df), UINT16_C(0xf004), UINT32_C(0x1000),
              registers, &load_address) == 1 &&
          load_address == UINT32_C(0x1008),
          "plan Thumb-2 literal LDR PC address");
    check(uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xf804), UINT32_C(0x1000),
              registers, &load_address) < 0 &&
          uvdb_thumb32_plan_load_pc_address(
              UINT16_C(0xf850), UINT16_C(0xf02d), UINT32_C(0x1000),
              registers, &load_address) < 0,
          "reject reserved and SP-offset Thumb-2 LDR PC forms");

    check_step_target(uvdb_thumb32_plan_branch_step(
                          UINT16_C(0xf040), UINT16_C(0x8004),
                          UINT32_C(0x1000), 0, &target),
                      &target, UINT32_C(0x100c), 2,
                      "plan taken Thumb-2 conditional B.W");
    check_step_target(uvdb_thumb32_plan_branch_step(
                          UINT16_C(0xf040), UINT16_C(0x8004),
                          UINT32_C(0x1000), UINT32_C(1) << 30,
                          &target),
                      &target, UINT32_C(0x1004), 2,
                      "plan untaken Thumb-2 conditional B.W");
    check_step_target(uvdb_thumb32_plan_branch_step(
                          UINT16_C(0xf47f), UINT16_C(0xaffe),
                          UINT32_C(0x1000), UINT32_C(1) << 30,
                          &target),
                      &target, UINT32_C(0x1004), 2,
                      "inactive Thumb-2 self-branch avoids current PC");
    check(uvdb_thumb32_plan_branch_step(
              UINT16_C(0xf47f), UINT16_C(0xaffe), UINT32_C(0x1000),
              0, &target) < 0,
          "reject unimplementable taken Thumb-2 self-branch step");
    check_step_target(uvdb_thumb32_plan_branch_step(
                          UINT16_C(0xf000), UINT16_C(0xb804),
                          UINT32_C(0x1000), 0, &target),
                      &target, UINT32_C(0x100c), 2,
                      "plan Thumb-2 unconditional B.W");
    check_step_target(uvdb_thumb32_plan_branch_step(
                          UINT16_C(0xf000), UINT16_C(0xe800),
                          UINT32_C(0x1002), 0, &target),
                      &target, UINT32_C(0x1004), 4,
                      "plan Thumb-2 BLX immediate with aligned ARM base");
    check(uvdb_thumb32_plan_branch_step(
              UINT16_C(0xe8d0), UINT16_C(0xf000), UINT32_C(0x1000),
              0, &target) == 0 &&
          uvdb_thumb32_plan_branch_step(
              UINT16_C(0xf000), UINT16_C(0xb804), UINT32_C(0x1001),
              0, &target) < 0 &&
          uvdb_thumb32_plan_branch_step(
              UINT16_C(0xf000), UINT16_C(0xb804), UINT32_C(0x1000),
              0, NULL) < 0,
          "validate Thumb-2 branch planner inputs and opcode class");
    check(uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xf850), UINT16_C(0xf000)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xf850), UINT16_C(0xfc04)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xf8d0), UINT16_C(0xf004)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xe8bd), UINT16_C(0x8000)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xf3de), UINT16_C(0x8f04)),
          "identify Thumb-2 LDR/POP/exception-return writes to PC");
    check(!uvdb_thumb32_instruction_may_write_pc(
               UINT16_C(0xf850), UINT16_C(0x0000)) &&
          !uvdb_thumb32_instruction_may_write_pc(
               UINT16_C(0xf8d0), UINT16_C(0x0004)) &&
          !uvdb_thumb32_instruction_may_write_pc(
               UINT16_C(0xe8bd), UINT16_C(0x4000)) &&
          !uvdb_thumb32_instruction_may_write_pc(
               UINT16_C(0xf3de), UINT16_C(0x3f04)),
          "leave non-PC Thumb-2 load/LDM forms unclassified");
    check(uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xe9b0), UINT16_C(0xc000)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xe811), UINT16_C(0xc000)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xea4f), UINT16_C(0x0f00)) &&
          uvdb_thumb32_instruction_may_write_pc(
              UINT16_C(0xf3c0), UINT16_C(0x8f00)),
          "classify Thumb-2 RFE, MOV-PC, and BXJ for fail-closed stepping");

    check(uvdb_thumb_it_step_placement_valid(
              UINT16_C(0x4487), 0, 2, 0x08) &&
          !uvdb_thumb_it_step_placement_valid(
              UINT16_C(0x4487), 0, 2, 0x04) &&
          !uvdb_thumb_it_step_placement_valid(
              UINT16_C(0xd100), 0, 2, 0x04) &&
          !uvdb_thumb_it_step_placement_valid(
              UINT16_C(0xf8d0), UINT16_C(0xf004), 4, 0x04) &&
          uvdb_thumb_it_step_placement_valid(
              UINT16_C(0x2000), 0, 2, 0x04),
          "allow PC writes only in the final IT slot");
    check(!uvdb_thumb_it_step_placement_valid(
              UINT16_C(0xb100), 0, 2, 0x08) &&
          uvdb_thumb_it_step_placement_valid(
              UINT16_C(0xb100), 0, 2, 0) &&
          !uvdb_thumb_it_step_placement_valid(
              UINT16_C(0x2000), 0, 3, 0),
          "reject CBZ in IT and malformed placement inputs");

    check(uvdb_step_instruction_may_block(UINT32_C(0x0000df7f), 1) &&
          uvdb_step_instruction_may_block(UINT32_C(0x0000bf20), 1) &&
          uvdb_step_instruction_may_block(UINT32_C(0x0000bf30), 1) &&
          uvdb_step_instruction_may_block(UINT32_C(0x8002f3af), 1) &&
          uvdb_step_instruction_may_block(UINT32_C(0x8003f3af), 1) &&
          uvdb_step_instruction_may_block(UINT32_C(0xef123456), 0) &&
          uvdb_step_instruction_may_block(UINT32_C(0x1320f002), 0) &&
          uvdb_step_instruction_may_block(UINT32_C(0xe320f003), 0),
          "selected-thread stepping identifies syscall and wait instructions");
    check(!uvdb_step_instruction_may_block(UINT32_C(0x0000bf00), 1) &&
          !uvdb_step_instruction_may_block(UINT32_C(0x0000be00), 1) &&
          !uvdb_step_instruction_may_block(UINT32_C(0x8000f3af), 1) &&
          !uvdb_step_instruction_may_block(UINT32_C(0xe1a00000), 0),
          "ordinary and breakpoint encodings do not block");
    check(uvdb_arm_instruction_starts_exclusive(
              UINT32_C(0xe1910f9f)) &&
          uvdb_arm_instruction_starts_exclusive(
              UINT32_C(0x01910f9f)) &&
          !uvdb_arm_instruction_starts_exclusive(
              UINT32_C(0xe1810f90)) &&
          uvdb_thumb32_instruction_starts_exclusive(
              UINT16_C(0xe851), UINT16_C(0x0f00)) &&
          uvdb_thumb32_instruction_starts_exclusive(
              UINT16_C(0xe8d1), UINT16_C(0x0f4f)) &&
          !uvdb_thumb32_instruction_starts_exclusive(
              UINT16_C(0xe8d0), UINT16_C(0xf000)),
          "recognize LDREX families without confusing table branches");
    check(uvdb_step_cpsr_state_supported(0) &&
          uvdb_step_cpsr_state_supported(UINT32_C(1) << 5) &&
          !uvdb_step_cpsr_state_supported(UINT32_C(1) << 24) &&
          !uvdb_step_cpsr_state_supported(UINT32_C(1) << 9) &&
          !uvdb_step_cpsr_state_supported((UINT32_C(1) << 24) |
                                           (UINT32_C(1) << 5)),
          "support little-endian ARM/Thumb but reject Jazelle, ThumbEE, and big-endian data state");
    check(uvdb_step_word_address_valid(UINT32_C(0x4000)) &&
          !uvdb_step_word_address_valid(UINT32_C(0x4001)) &&
          !uvdb_step_word_address_valid(UINT32_C(0x4002)),
          "protected step-target reads require word alignment");

    uint32_t split_itstate = (UINT32_C(0x8c) << 8) |
                             (UINT32_C(1) << 25);
    check(uvdb_thumb_itstate_from_cpsr(split_itstate) == 0x8d,
          "extract split CPSR Thumb ITSTATE");
    check(uvdb_thumb_itstate_advance(0x0c) == 0x18 &&
          uvdb_thumb_itstate_advance(0x18) == 0 &&
          uvdb_thumb_itstate_advance(0x100) == 0,
          "advance and terminate bounded Thumb ITSTATE");

    check(!uvdb_stop_cleanup_can_release(0, 1),
          "uncertain stop with UDF patches remains fail-closed");
    check(uvdb_stop_cleanup_can_release(1, 1),
          "coherent stop permits restore-before-release cleanup");
    check(uvdb_stop_cleanup_can_release(0, 0),
          "uncertain stop without patches permits bounded release cleanup");
    check(!uvdb_stop_cleanup_can_release(-1, 1) &&
          !uvdb_stop_cleanup_can_release(0, -1),
          "unknown cleanup state remains fail-closed");

    if(failures)
        return 1;
    puts("PASS: coherent RSP thread selection and vCont planning");
    return 0;
}
