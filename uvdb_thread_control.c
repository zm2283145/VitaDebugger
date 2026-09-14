#include "uvdb_thread_control.h"

#include <limits.h>
#include <string.h>

#define UVDB_VCONT_MAX_ACTIONS (UVDB_THREAD_INVENTORY_CAPACITY + 1u)

struct uvdb_vcont_action {
    enum uvdb_resume_kind kind;
    int32_t thread;
    unsigned int has_thread;
};

static int thread_inventory_valid(
    const struct uvdb_thread_inventory* inventory)
{
    if(!inventory || inventory->count > UVDB_THREAD_INVENTORY_CAPACITY)
        return 0;
    for(size_t i = 0; i < inventory->count; ++i)
    {
        if(inventory->ids[i] <= 0)
            return 0;
        for(size_t j = 0; j < i; ++j)
            if(inventory->ids[j] == inventory->ids[i])
                return 0;
    }
    return 1;
}

void uvdb_thread_inventory_reset(struct uvdb_thread_inventory* inventory)
{
    if(inventory)
        memset(inventory, 0, sizeof(*inventory));
}

int uvdb_thread_inventory_contains(
    const struct uvdb_thread_inventory* inventory,
    int32_t id)
{
    if(!thread_inventory_valid(inventory) || id <= 0)
        return 0;
    for(size_t i = 0; i < inventory->count; ++i)
        if(inventory->ids[i] == id)
            return 1;
    return 0;
}

int uvdb_thread_inventory_add(struct uvdb_thread_inventory* inventory,
                              int32_t id)
{
    if(!thread_inventory_valid(inventory) || id <= 0)
        return -1;
    if(uvdb_thread_inventory_contains(inventory, id))
        return 0;
    if(inventory->count >= UVDB_THREAD_INVENTORY_CAPACITY)
        return -1;
    inventory->ids[inventory->count++] = id;
    return 1;
}

void uvdb_thread_selection_reset(struct uvdb_thread_selection* selection)
{
    if(!selection)
        return;
    selection->stopped = UVDB_RSP_THREAD_ALL;
    selection->general = UVDB_RSP_THREAD_ANY;
    selection->resume = UVDB_RSP_THREAD_ALL;
}

void uvdb_thread_selection_reconcile(
    struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !thread_inventory_valid(inventory))
        return;
    if(selection->stopped > 0 &&
       !uvdb_thread_inventory_contains(inventory, selection->stopped))
        selection->stopped = UVDB_RSP_THREAD_ALL;
    /*
     * Keep explicit Hg/Hc selectors even after their thread disappears.  The
     * next operation then fails closed instead of silently widening a stale
     * per-thread request to the stopped thread or the whole process.
     */
}

void uvdb_thread_selection_note_stop(
    struct uvdb_thread_selection* selection,
    int32_t stopped_thread,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection)
        return;
    selection->stopped = stopped_thread > 0 ? stopped_thread
                                            : UVDB_RSP_THREAD_ALL;
    if(inventory)
        uvdb_thread_selection_reconcile(selection, inventory);
}

void uvdb_thread_selection_note_resume(
    struct uvdb_thread_selection* selection)
{
    if(selection)
        selection->stopped = UVDB_RSP_THREAD_ALL;
}

static int hex_value(char value)
{
    if(value >= '0' && value <= '9')
        return value - '0';
    if(value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if(value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

int uvdb_rsp_parse_u32_hex(const char* text, size_t size, uint32_t* value)
{
    if(!text || !value || !size || size > 8)
        return -1;

    uint32_t parsed = 0;
    for(size_t i = 0; i < size; ++i)
    {
        int digit = hex_value(text[i]);
        if(digit < 0)
            return -1;
        if(parsed > (UINT32_MAX - (uint32_t)digit) / 16u)
            return -1;
        parsed = parsed * 16u + (uint32_t)digit;
    }
    *value = parsed;
    return 0;
}

int uvdb_rsp_parse_thread_id(const char* text, size_t size, int32_t* id)
{
    if(!text || !id || !size)
        return -1;
    if(size == 2 && text[0] == '-' && text[1] == '1')
    {
        *id = UVDB_RSP_THREAD_ALL;
        return 0;
    }

    uint32_t value;
    if(uvdb_rsp_parse_u32_hex(text, size, &value) < 0 || value > INT32_MAX)
        return -1;
    *id = (int32_t)value;
    return 0;
}

int uvdb_thread_selection_apply(
    struct uvdb_thread_selection* selection,
    char operation,
    const char* text,
    size_t size,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !thread_inventory_valid(inventory) ||
       inventory->count == 0 ||
       (operation != 'g' && operation != 'c'))
        return -1;
    int32_t id;
    if(uvdb_rsp_parse_thread_id(text, size, &id) < 0)
        return -1;
    if(id > 0 && !uvdb_thread_inventory_contains(inventory, id))
        return -1;
    if(operation == 'g')
        selection->general = id;
    else
        selection->resume = id;
    return 0;
}

static int32_t resolve_selector(
    int32_t selector,
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!thread_inventory_valid(inventory) || inventory->count == 0 ||
       !selection || selector < UVDB_RSP_THREAD_ALL ||
       selection->stopped < UVDB_RSP_THREAD_ALL)
        return UVDB_RSP_THREAD_ALL;
    if(selector > 0 && uvdb_thread_inventory_contains(inventory, selector))
        return selector;
    if(selector > 0)
        return UVDB_RSP_THREAD_ALL;
    if(selection && selection->stopped > 0 &&
       uvdb_thread_inventory_contains(inventory, selection->stopped))
        return selection->stopped;
    if(inventory && inventory->count)
        return inventory->ids[0];
    return UVDB_RSP_THREAD_ALL;
}

int32_t uvdb_thread_selection_general(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory)
        return UVDB_RSP_THREAD_ALL;
    return resolve_selector(selection->general, selection, inventory);
}

int32_t uvdb_thread_selection_step(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory)
        return UVDB_RSP_THREAD_ALL;
    return resolve_selector(selection->resume, selection, inventory);
}

int uvdb_thread_selection_plan_legacy(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory,
    int stepping,
    struct uvdb_resume_plan* plan)
{
    if(!selection || !thread_inventory_valid(inventory) || !plan ||
       inventory->count == 0 ||
       selection->resume < UVDB_RSP_THREAD_ALL)
        return -1;
    if(selection->resume > 0)
        return -1;
    if(stepping && selection->resume == UVDB_RSP_THREAD_ALL &&
       inventory->count > 1)
        return -1;

    int32_t selected = uvdb_thread_selection_step(selection, inventory);
    if(selected <= 0)
        return -1;
    plan->kind = stepping ? UVDB_RESUME_STEP : UVDB_RESUME_CONTINUE;
    plan->step_thread = stepping ? selected : UVDB_RSP_THREAD_ALL;
    return 0;
}

static int parse_vcont_action(
    const char* text,
    size_t size,
    struct uvdb_vcont_action* action)
{
    if(!text || !size || !action)
        return -1;
    if(text[0] == 'c')
        action->kind = UVDB_RESUME_CONTINUE;
    else if(text[0] == 's')
        action->kind = UVDB_RESUME_STEP;
    else
        return -1;

    action->thread = UVDB_RSP_THREAD_ALL;
    action->has_thread = 0;
    if(size == 1)
        return 0;
    if(size < 3 || text[1] != ':')
        return -1;
    if(uvdb_rsp_parse_thread_id(text + 2, size - 2, &action->thread) < 0)
        return -1;
    action->has_thread = 1;
    return 0;
}

static int action_matches(
    const struct uvdb_vcont_action* action,
    int32_t thread,
    int32_t any_thread)
{
    if(!action->has_thread || action->thread == UVDB_RSP_THREAD_ALL)
        return 1;
    if(action->thread == UVDB_RSP_THREAD_ANY)
        return thread == any_thread;
    return action->thread == thread;
}

int uvdb_rsp_parse_vcont(
    const char* packet,
    size_t size,
    const struct uvdb_thread_inventory* inventory,
    struct uvdb_resume_plan* plan)
{
    static const char prefix[] = "vCont;";
    if(!packet || !thread_inventory_valid(inventory) || !plan ||
       inventory->count == 0 ||
       size <= sizeof(prefix) - 1 ||
       memcmp(packet, prefix, sizeof(prefix) - 1) != 0)
        return -1;

    struct uvdb_vcont_action actions[UVDB_VCONT_MAX_ACTIONS];
    size_t action_count = 0;
    size_t cursor = sizeof(prefix) - 1;
    while(cursor < size)
    {
        size_t end = cursor;
        while(end < size && packet[end] != ';')
            ++end;
        if(end == cursor || action_count >= UVDB_VCONT_MAX_ACTIONS ||
           parse_vcont_action(packet + cursor, end - cursor,
                              &actions[action_count]) < 0)
            return -1;
        if(actions[action_count].has_thread &&
           actions[action_count].thread > 0 &&
           !uvdb_thread_inventory_contains(inventory,
                                            actions[action_count].thread))
            return -1;
        ++action_count;
        if(end == size)
            break;
        cursor = end + 1;
        if(cursor == size)
            return -1;
    }

    size_t step_count = 0;
    int32_t step_thread = UVDB_RSP_THREAD_ALL;
    int32_t any_thread = inventory->ids[0];
    for(size_t thread_index = 0; thread_index < inventory->count;
        ++thread_index)
    {
        int matched = 0;
        for(size_t action_index = 0; action_index < action_count;
            ++action_index)
        {
            if(!action_matches(&actions[action_index],
                               inventory->ids[thread_index], any_thread))
                continue;
            matched = 1;
            if(actions[action_index].kind == UVDB_RESUME_STEP)
            {
                ++step_count;
                step_thread = inventory->ids[thread_index];
            }
            break;
        }
        if(!matched || step_count > 1)
            return -1;
    }

    plan->kind = step_count ? UVDB_RESUME_STEP : UVDB_RESUME_CONTINUE;
    plan->step_thread = step_thread;
    return 0;
}

int uvdb_arm_condition_passed(unsigned int condition, uint32_t cpsr)
{
    if(condition > 0xfu || condition == 0xfu)
        return 0;

    int n = (int)((cpsr >> 31) & 1u);
    int z = (int)((cpsr >> 30) & 1u);
    int c = (int)((cpsr >> 29) & 1u);
    int v = (int)((cpsr >> 28) & 1u);
    int passed;
    switch(condition >> 1)
    {
        case 0: passed = z; break;
        case 1: passed = c; break;
        case 2: passed = n; break;
        case 3: passed = v; break;
        case 4: passed = c && !z; break;
        case 5: passed = n == v; break;
        case 6: passed = !z && n == v; break;
        default: passed = condition == 0xeu; break;
    }
    return (condition & 1u) ? !passed : passed;
}

static int set_arm_step_target(
    uint32_t address,
    int thumb,
    uint32_t current_pc,
    struct uvdb_step_target* target)
{
    target->breakpoint_size = thumb ? 2u : 4u;
    target->address = address & (thumb ? ~UINT32_C(1) : ~UINT32_C(3));
    if(target->address == current_pc)
        return -1;
    return 1;
}

static int32_t sign_extend_u32(uint32_t value, unsigned int bits)
{
    uint32_t sign = UINT32_C(1) << (bits - 1u);
    return (int32_t)(value ^ sign) - (int32_t)sign;
}

int uvdb_arm_plan_direct_step(
    uint32_t instruction,
    uint32_t pc,
    uint32_t cpsr,
    const uint32_t registers[16],
    struct uvdb_step_target* target)
{
    if(!registers || !target || (pc & 3u))
        return -1;

    /* A32 BLX immediate uses cond=0xf and bit 24 as the low encoded offset
     * bit. It always exchanges to Thumb state. Decode it before B/BL so it
     * cannot be mistaken for an ordinary conditional branch. */
    if((instruction & UINT32_C(0xfe000000)) == UINT32_C(0xfa000000))
    {
        uint32_t encoded = ((instruction & UINT32_C(0x00ffffff)) << 2) |
                           ((instruction >> 23) & 2u);
        int32_t offset = sign_extend_u32(encoded, 26);
        return set_arm_step_target(pc + 8u + (uint32_t)offset, 1, pc,
                                   target);
    }

    /* A32 B/BL immediate. The saved flags identify the one path the stopped
     * instruction can take, avoiding a speculative patch at an unmapped path. */
    if((instruction & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000))
    {
        unsigned int condition = instruction >> 28;
        if(condition == 0xfu)
            return 0;
        if(!uvdb_arm_condition_passed(condition, cpsr))
            return set_arm_step_target(pc + 4u, 0, pc, target);
        int32_t offset = sign_extend_u32(
            (instruction & UINT32_C(0x00ffffff)) << 2, 26);
        return set_arm_step_target(pc + 8u + (uint32_t)offset, 0, pc,
                                   target);
    }

    /* A32 MOV PC,Rm with an unshifted register operand. ARMv7 ALUWritePC uses
     * BXWritePC in ARM state, so bit zero selects the destination state. */
    if((instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x01a0f000))
    {
        unsigned int condition = instruction >> 28;
        if(condition == 0xfu)
            return 0;
        if(!uvdb_arm_condition_passed(condition, cpsr))
            return set_arm_step_target(pc + 4u, 0, pc, target);
        unsigned int rm = instruction & 0xfu;
        uint32_t address = rm == 15u ? pc + 8u : registers[rm];
        return set_arm_step_target(address, (address & 1u) != 0, pc, target);
    }

    /* A32 BX/BLX register. Conditions apply to these encodings too, and R15
     * reads as the architectural PC (current instruction address plus eight). */
    if((instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff10) ||
       (instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff30))
    {
        unsigned int condition = instruction >> 28;
        if(condition == 0xfu)
            return 0;
        if(!uvdb_arm_condition_passed(condition, cpsr))
            return set_arm_step_target(pc + 4u, 0, pc, target);
        unsigned int rm = instruction & 0xfu;
        uint32_t address = rm == 15u ? pc + 8u : registers[rm];
        return set_arm_step_target(address, (address & 1u) != 0, pc, target);
    }

    return 0;
}

int uvdb_arm_instruction_may_write_pc(uint32_t instruction)
{
    unsigned int condition = instruction >> 28;

    if((instruction & UINT32_C(0xfe000000)) == UINT32_C(0xfa000000) ||
       (condition != 0xfu &&
        (instruction & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000)) ||
       (condition != 0xfu &&
        ((instruction & UINT32_C(0x0ffffff0)) ==
             UINT32_C(0x012fff10) ||
         (instruction & UINT32_C(0x0ffffff0)) ==
             UINT32_C(0x012fff20) ||
         (instruction & UINT32_C(0x0ffffff0)) ==
             UINT32_C(0x012fff30))))
        return 1;

    /* LDM/POP with PC in the register list. */
    if(condition != 0xfu &&
       (instruction & UINT32_C(0x0e100000)) == UINT32_C(0x08100000) &&
       (instruction & UINT32_C(0x00008000)))
        return 1;

    /* LDR PC, including the register-offset forms the live decoder cannot yet
     * safely evaluate. cond=0xf encodes memory hints rather than LDR PC. */
    if(condition != 0xfu &&
       (instruction & UINT32_C(0x0c10f000)) == UINT32_C(0x0410f000))
        return 1;

    /* Data-processing writes to R15. Restrict the register form to valid
     * shifter shapes so multiply/extra-load-store encodings are not mistaken
     * for ordinary ALU operations. */
    unsigned int opcode_class = (instruction >> 25) & 7u;
    if(condition != 0xfu && (instruction & UINT32_C(0x0000f000)) ==
                                  UINT32_C(0x0000f000) &&
       (opcode_class == 1u ||
        (opcode_class == 0u &&
         (!(instruction & UINT32_C(0x10)) ||
          !(instruction & UINT32_C(0x80))))))
        return 1;

    return 0;
}

int uvdb_thumb16_plan_direct_step(
    uint16_t instruction,
    uint32_t pc,
    uint32_t cpsr,
    const uint32_t registers[16],
    struct uvdb_step_target* target)
{
    if(!registers || !target || (pc & 1u))
        return -1;

    /* Conditional B (excluding SVC and permanently undefined encodings). */
    if((instruction & UINT16_C(0xf000)) == UINT16_C(0xd000) &&
       (instruction & UINT16_C(0x0f00)) < UINT16_C(0x0e00))
    {
        unsigned int condition = (instruction >> 8) & 0xfu;
        uint32_t encoded = (uint32_t)(instruction & UINT16_C(0x00ff)) << 1;
        int32_t offset = sign_extend_u32(encoded, 9);
        uint32_t address = uvdb_arm_condition_passed(condition, cpsr)
            ? pc + 4u + (uint32_t)offset : pc + 2u;
        return set_arm_step_target(address, 1, pc, target);
    }

    /* Unconditional 16-bit B. */
    if((instruction & UINT16_C(0xf800)) == UINT16_C(0xe000))
    {
        uint32_t encoded = (uint32_t)(instruction & UINT16_C(0x07ff)) << 1;
        int32_t offset = sign_extend_u32(encoded, 12);
        return set_arm_step_target(pc + 4u + (uint32_t)offset, 1, pc,
                                   target);
    }

    /* CBZ/CBNZ use a register value rather than APSR flags. */
    if((instruction & UINT16_C(0xf500)) == UINT16_C(0xb100))
    {
        unsigned int rn = instruction & 7u;
        int nonzero = (instruction & UINT16_C(0x0800)) != 0;
        int take = nonzero ? registers[rn] != 0 : registers[rn] == 0;
        uint32_t address = take
            ? pc + 4u + ((instruction & UINT16_C(0x0200)) >> 3) +
                  ((instruction & UINT16_C(0x00f8)) >> 2)
            : pc + 2u;
        return set_arm_step_target(address, 1, pc, target);
    }

    return 0;
}

int uvdb_thumb32_plan_branch_step(
    uint16_t first,
    uint16_t second,
    uint32_t pc,
    uint32_t cpsr,
    struct uvdb_step_target* target)
{
    if(!target || (pc & 1u))
        return -1;
    if((first & UINT16_C(0xf800)) != UINT16_C(0xf000) ||
       !(second & UINT16_C(0x8000)))
        return 0;

    /* B.W, BL, and BLX immediate. J1/J2 encode complemented I1/I2. */
    if((second & UINT16_C(0x1000)) != 0 ||
       (second & UINT16_C(0xd001)) == UINT16_C(0xc000))
    {
        uint32_t sign = (first >> 10) & 1u;
        uint32_t j1 = (second >> 13) & 1u;
        uint32_t j2 = (second >> 11) & 1u;
        uint32_t i1 = !(j1 ^ sign);
        uint32_t i2 = !(j2 ^ sign);
        uint32_t encoded = (sign << 24) | (i1 << 23) | (i2 << 22) |
                           ((uint32_t)(first & UINT16_C(0x03ff)) << 12) |
                           ((uint32_t)(second & UINT16_C(0x07ff)) << 1);
        int32_t offset = sign_extend_u32(encoded, 25);
        int exchange_to_arm = (second & UINT16_C(0x1000)) == 0;
        uint32_t base = exchange_to_arm ? (pc + 4u) & ~UINT32_C(3)
                                        : pc + 4u;
        return set_arm_step_target(base + (uint32_t)offset,
                                   !exchange_to_arm, pc, target);
    }

    /* Conditional B.W directly carries S:J2:J1:imm6:imm11:'0'. */
    if((second & UINT16_C(0xd000)) == UINT16_C(0x8000) &&
       (first & UINT16_C(0x0380)) != UINT16_C(0x0380))
    {
        unsigned int condition = (first >> 6) & 0xfu;
        uint32_t encoded = (((uint32_t)first >> 10) & 1u) << 20 |
                           (((uint32_t)second >> 11) & 1u) << 19 |
                           (((uint32_t)second >> 13) & 1u) << 18 |
                           ((uint32_t)(first & UINT16_C(0x003f)) << 12) |
                           ((uint32_t)(second & UINT16_C(0x07ff)) << 1);
        int32_t offset = sign_extend_u32(encoded, 21);
        uint32_t address = uvdb_arm_condition_passed(condition, cpsr)
            ? pc + 4u + (uint32_t)offset : pc + 4u;
        return set_arm_step_target(address, 1, pc, target);
    }

    return 0;
}

int uvdb_thumb16_instruction_may_write_pc(uint16_t instruction)
{
    if(((instruction & UINT16_C(0xf000)) == UINT16_C(0xd000) &&
        (instruction & UINT16_C(0x0f00)) < UINT16_C(0x0e00)) ||
       (instruction & UINT16_C(0xf800)) == UINT16_C(0xe000) ||
       (instruction & UINT16_C(0xff00)) == UINT16_C(0x4700) ||
       (instruction & UINT16_C(0xff00)) == UINT16_C(0xbd00))
        return 1;

    if((instruction & UINT16_C(0xff00)) == UINT16_C(0x4400) ||
       (instruction & UINT16_C(0xff00)) == UINT16_C(0x4600))
    {
        unsigned int rd = (instruction & 7u) |
                          ((instruction >> 4) & 8u);
        return rd == 15u;
    }
    return 0;
}

int uvdb_thumb32_instruction_may_write_pc(
    uint16_t first,
    uint16_t second)
{
    if((first & UINT16_C(0xf800)) == UINT16_C(0xf000) &&
       (second & UINT16_C(0x8000)) &&
       ((second & UINT16_C(0x1000)) != 0 ||
        (second & UINT16_C(0xd001)) == UINT16_C(0xc000) ||
        ((second & UINT16_C(0xd000)) == UINT16_C(0x8000) &&
         (first & UINT16_C(0x0380)) != UINT16_C(0x0380))))
        return 1;
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe8d0) &&
       (second & UINT16_C(0xffe0)) == UINT16_C(0xf000))
        return 1;
    if(((first & UINT16_C(0xffd0)) == UINT16_C(0xe890) ||
        (first & UINT16_C(0xffd0)) == UINT16_C(0xe910)) &&
       (second & UINT16_C(0x8000)))
        return 1;
    if(((first & UINT16_C(0xfff0)) == UINT16_C(0xf8d0) ||
        (first & UINT16_C(0xfff0)) == UINT16_C(0xf850)) &&
       (second & UINT16_C(0xf000)) == UINT16_C(0xf000))
        return 1;
    if(first == UINT16_C(0xf3de) &&
       (second & UINT16_C(0xff00)) == UINT16_C(0x8f00))
        return 1;
    return 0;
}

unsigned int uvdb_thumb_itstate_from_cpsr(uint32_t cpsr)
{
    return ((cpsr >> 8) & 0xfcu) | ((cpsr >> 25) & 3u);
}

unsigned int uvdb_thumb_itstate_advance(unsigned int itstate)
{
    if(itstate > 0xffu || (itstate & 7u) == 0)
        return 0;
    return (itstate & 0xe0u) | ((itstate << 1) & 0x1fu);
}

int uvdb_stop_cleanup_can_release(int coherent_stop, int breakpoints_active)
{
    /* Only the two proven boolean states authorize release. Corrupted or
     * unknown policy inputs must not be interpreted as truthy/falsey in the
     * direction that could resume a process over an executable UDF patch. */
    return coherent_stop == 1 || breakpoints_active == 0;
}
