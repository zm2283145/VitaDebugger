#pragma once

#include <stddef.h>
#include <stdint.h>

#define UVDB_THREAD_INVENTORY_CAPACITY 64u
#define UVDB_RSP_THREAD_ANY ((int32_t)0)
#define UVDB_RSP_THREAD_ALL ((int32_t)-1)

struct uvdb_thread_inventory {
    int32_t ids[UVDB_THREAD_INVENTORY_CAPACITY];
    size_t count;
};

struct uvdb_thread_selection {
    int32_t stopped;
    int32_t general;
    int32_t resume;
};

enum uvdb_resume_kind {
    UVDB_RESUME_CONTINUE = 1,
    UVDB_RESUME_STEP = 2,
};

struct uvdb_resume_plan {
    enum uvdb_resume_kind kind;
    int32_t step_thread;
};

struct uvdb_step_target {
    uint32_t address;
    uint8_t breakpoint_size;
};

void uvdb_thread_inventory_reset(struct uvdb_thread_inventory* inventory);
int uvdb_thread_inventory_add(struct uvdb_thread_inventory* inventory,
                              int32_t id);
int uvdb_thread_inventory_contains(
    const struct uvdb_thread_inventory* inventory,
    int32_t id);

void uvdb_thread_selection_reset(struct uvdb_thread_selection* selection);
void uvdb_thread_selection_note_stop(
    struct uvdb_thread_selection* selection,
    int32_t stopped_thread,
    const struct uvdb_thread_inventory* inventory);
void uvdb_thread_selection_note_resume(
    struct uvdb_thread_selection* selection);
void uvdb_thread_selection_reconcile(
    struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory);

/*
 * Parse one non-multiprocess RSP thread-id. VitaDebugger does not advertise
 * multiprocess support, so pPID.TID forms are rejected rather than partially
 * accepted. Positive IDs, 0 (any), and -1 (all) are supported exactly.
 */
int uvdb_rsp_parse_thread_id(const char* text, size_t size, int32_t* id);

/* Parse one exact target-width hexadecimal value with no trailing syntax. */
int uvdb_rsp_parse_u32_hex(const char* text, size_t size, uint32_t* value);

/* Apply an Hg/Hc selector after exact thread-id parsing and visibility checks. */
int uvdb_thread_selection_apply(
    struct uvdb_thread_selection* selection,
    char operation,
    const char* text,
    size_t size,
    const struct uvdb_thread_inventory* inventory);

int32_t uvdb_thread_selection_general(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory);
int32_t uvdb_thread_selection_step(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory);

/*
 * Reduce a legacy c/s request after Hc selection. A positive Hc requests a
 * selective resume that the current all-stop backend cannot provide, so that
 * combination is rejected instead of being widened to process-wide execution.
 * Hc-1 cannot request a multi-thread legacy step. Thread-specific all-stop
 * stepping remains expressible as vCont;s:T;c.
 */
int uvdb_thread_selection_plan_legacy(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory,
    int stepping,
    struct uvdb_resume_plan* plan);

/*
 * Parse and reduce an all-stop vCont action list. VitaDebugger advertises only
 * c and s. The current stop boundary can resume the process as a whole, so a
 * valid plan must cover every inventoried thread and may select at most one
 * thread for stepping. Leftmost-match semantics are preserved.
 */
int uvdb_rsp_parse_vcont(
    const char* packet,
    size_t size,
    const struct uvdb_thread_inventory* inventory,
    struct uvdb_resume_plan* plan);

/* Evaluate an A32 condition code against one saved CPSR. Reserved condition
 * 0xf is deliberately false here; the unconditional BLX-immediate encoding is
 * handled explicitly by uvdb_arm_plan_direct_step(). */
int uvdb_arm_condition_passed(unsigned int condition, uint32_t cpsr);

/* Plan direct A32 control flow without reading target memory. Returns 1 when
 * the instruction was handled, 0 when the caller should use another decoder
 * or normal PC+4 fallthrough, and -1 for invalid arguments. The returned
 * address is canonicalized for its selected ARM/Thumb instruction set. */
int uvdb_arm_plan_direct_step(
    uint32_t instruction,
    uint32_t pc,
    uint32_t cpsr,
    const uint32_t registers[16],
    struct uvdb_step_target* target);

/* Conservatively identify A32 instructions whose sequential PC+4 trap could
 * be bypassed. Callers can reject any such form they have not decoded. */
int uvdb_arm_instruction_may_write_pc(uint32_t instruction);

/* Plan the host-decodable 16-bit Thumb branches. The same return convention
 * as uvdb_arm_plan_direct_step applies. */
int uvdb_thumb16_plan_direct_step(
    uint16_t instruction,
    uint32_t pc,
    uint32_t cpsr,
    const uint32_t registers[16],
    struct uvdb_step_target* target);

/* Plan Thumb-2 B.W, BL, BLX-immediate, and conditional B.W pairs. */
int uvdb_thumb32_plan_branch_step(
    uint16_t first,
    uint16_t second,
    uint32_t pc,
    uint32_t cpsr,
    struct uvdb_step_target* target);

/* Identify Thumb control transfers that must not use a sequential fallback. */
int uvdb_thumb16_instruction_may_write_pc(uint16_t instruction);
int uvdb_thumb32_instruction_may_write_pc(
    uint16_t first,
    uint16_t second);

/* Extract and advance the split Thumb ITSTATE field stored in CPSR. */
unsigned int uvdb_thumb_itstate_from_cpsr(uint32_t cpsr);
unsigned int uvdb_thumb_itstate_advance(unsigned int itstate);

/* An uncertain stop may be released only when no executable UDF patch needs
 * all-stop protection. This small policy gate is shared with host regression
 * tests so cleanup cannot silently regress to EndStop-before-restore. */
int uvdb_stop_cleanup_can_release(int coherent_stop, int breakpoints_active);
