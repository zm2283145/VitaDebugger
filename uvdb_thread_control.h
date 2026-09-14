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

enum uvdb_resume_scope {
    UVDB_RESUME_SCOPE_PROCESS = 1,
    UVDB_RESUME_SCOPE_STOPPED_THREAD = 2,
};

struct uvdb_resume_plan {
    enum uvdb_resume_kind kind;
    int32_t step_thread;
    enum uvdb_resume_scope scope;
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
 * Reduce a legacy c/s request after Hc selection. In all-stop mode GDB uses a
 * positive Hc followed by `s` for automatic software-breakpoint step-over.
 * The stopped exception thread can execute one instruction while the active
 * kernel stop token keeps every peer suspended. A positive-Hc continue stays
 * unsupported because it requests an unbounded selective execution. Hc-1
 * cannot request a multi-thread legacy step.
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

/* Validate the runtime facts required before a resume plan may mutate code.
 * Selected-thread scope is only safe for the current stopped exception thread
 * while a healthy kernel stop session owns every peer. */
int uvdb_resume_plan_validate(
    const struct uvdb_resume_plan* plan,
    int32_t exception_thread,
    int target_stopped,
    int stop_session_active,
    int stop_session_failed,
    int has_pc_override);

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

/* Resolve the word address read by a taken A32 LDR whose destination is PC.
 * Returns 1 for a decoded taken load, 0 for another instruction or a failed
 * condition, and -1 for a recognized but unsafe/reserved form. */
int uvdb_arm_plan_load_pc_address(
    uint32_t instruction,
    uint32_t pc,
    uint32_t cpsr,
    const uint32_t registers[16],
    uint32_t* load_address);

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

/* Resolve the word address read by a Thumb-2 LDR.W whose destination is PC.
 * The return convention matches uvdb_arm_plan_load_pc_address(). */
int uvdb_thumb32_plan_load_pc_address(
    uint16_t first,
    uint16_t second,
    uint32_t pc,
    const uint32_t registers[16],
    uint32_t* load_address);

/* Identify Thumb control transfers that must not use a sequential fallback. */
int uvdb_thumb16_instruction_may_write_pc(uint16_t instruction);
int uvdb_thumb32_instruction_may_write_pc(
    uint16_t first,
    uint16_t second);

/* Identify instructions that can sleep indefinitely while the debugger keeps
 * every peer suspended. The selected-thread step path rejects these until it
 * has a separate bounded cancellation mechanism. For Thumb-2, pack the first
 * halfword in bits 15:0 and the second halfword in bits 31:16. */
int uvdb_step_instruction_may_block(uint32_t instruction, int thumb);

/* Software traps immediately after LDREX clear the exclusive monitor and can
 * change the following STREX result. These helpers keep such sequences stopped
 * until a bounded LDREX..STREX planner is available. */
int uvdb_arm_instruction_starts_exclusive(uint32_t instruction);
int uvdb_thumb32_instruction_starts_exclusive(
    uint16_t first,
    uint16_t second);

/* Validate architectural IT placement before condition-based shortcuts. */
int uvdb_thumb_it_step_placement_valid(
    uint16_t first,
    uint16_t second,
    size_t instruction_size,
    unsigned int itstate);

/* LDR/LDM/POP PC resolution reads one architectural word. */
int uvdb_step_word_address_valid(uint32_t address);

/* Jazelle/ThumbEE and big-endian data execution are outside this decoder. */
int uvdb_step_cpsr_state_supported(uint32_t cpsr);

/* Extract and advance the split Thumb ITSTATE field stored in CPSR. */
unsigned int uvdb_thumb_itstate_from_cpsr(uint32_t cpsr);
unsigned int uvdb_thumb_itstate_advance(unsigned int itstate);

/* An uncertain stop may be released only when no executable UDF patch needs
 * all-stop protection. This small policy gate is shared with host regression
 * tests so cleanup cannot silently regress to EndStop-before-restore. */
int uvdb_stop_cleanup_can_release(int coherent_stop, int breakpoints_active);
